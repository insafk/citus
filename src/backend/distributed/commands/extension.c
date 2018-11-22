/*-------------------------------------------------------------------------
 *
 * extension.c
 *    Commands for creating and altering extensions.
 *
 * Copyright (c) 2018, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "citus_version.h"
#include "distributed/commands.h"
#include "distributed/metadata_cache.h"
#include "distributed/worker_protocol.h"
#include "libpq/libpq.h"
#include "nodes/parsenodes.h"
#include "openssl/dsa.h"
#include "openssl/ssl.h"
#include "openssl/pem.h"
#include "openssl/err.h"
#include "openssl/x509.h"
#include "postmaster/postmaster.h"
#include "utils/guc.h"

#define DirectFunctionCall0(func) \
	DirectFunctionCall0Coll(func, InvalidOid)
#define ENABLE_SSL_QUERY "ALTER SYSTEM SET ssl TO on;"
#define CITUS_AUTO_SSL_COMMON_NAME "citus-auto-ssl"
#define X509_SUBJECT_COMMON_NAME "CN"

/* Local functions forward declarations for helper functions */
static char * ExtractNewExtensionVersion(Node *parsetree);
static Datum DirectFunctionCall0Coll(PGFunction func, Oid collation);

#ifdef USE_SSL
static bool CreateCertificatesWhenNeeded(void);
static EVP_PKEY * generate_key(void);
static X509 * generate_x509(EVP_PKEY *pkey);
static bool write_to_disk(EVP_PKEY *pkey, X509 *x509);
#endif /* USE_SSL */

/* use pg's implementation that is not exposed in a header file */
extern Datum pg_reload_conf(PG_FUNCTION_ARGS);

/*
 * IsCitusExtensionStmt returns whether a given utility is a CREATE or ALTER
 * EXTENSION statement which references the citus extension. This function
 * returns false for all other inputs.
 */
bool
IsCitusExtensionStmt(Node *parsetree)
{
	char *extensionName = "";

	if (IsA(parsetree, CreateExtensionStmt))
	{
		extensionName = ((CreateExtensionStmt *) parsetree)->extname;
	}
	else if (IsA(parsetree, AlterExtensionStmt))
	{
		extensionName = ((AlterExtensionStmt *) parsetree)->extname;
	}

	return (strcmp(extensionName, "citus") == 0);
}


/*
 * ErrorIfUnstableCreateOrAlterExtensionStmt compares CITUS_EXTENSIONVERSION
 * and version given CREATE/ALTER EXTENSION statement will create/update to. If
 * they are not same in major or minor version numbers, this function errors
 * out. It ignores the schema version.
 */
void
ErrorIfUnstableCreateOrAlterExtensionStmt(Node *parsetree)
{
	char *newExtensionVersion = ExtractNewExtensionVersion(parsetree);

	if (newExtensionVersion != NULL)
	{
		/*  explicit version provided in CREATE or ALTER EXTENSION UPDATE; verify */
		if (!MajorVersionsCompatible(newExtensionVersion, CITUS_EXTENSIONVERSION))
		{
			ereport(ERROR, (errmsg("specified version incompatible with loaded "
								   "Citus library"),
							errdetail("Loaded library requires %s, but %s was specified.",
									  CITUS_MAJORVERSION, newExtensionVersion),
							errhint("If a newer library is present, restart the database "
									"and try the command again.")));
		}
	}
	else
	{
		/*
		 * No version was specified, so PostgreSQL will use the default_version
		 * from the citus.control file.
		 */
		CheckAvailableVersion(ERROR);
	}
}


void
ProcessCitusExtensionStmt(Node *parsetree)
{
	if (IsA(parsetree, CreateExtensionStmt))
	{
		/*
		 * during the creation of citus we check if ssl is on, if it is not on we will
		 * turn it on and generate a certificate with key if not already existing. This
		 * makes citus enctypt its traffic by default.
		 *
		 * Users are encouraged to create their own certificate chain for actual security
		 * as having auto generated certificates only work up-to sslmode require. Anything
		 * higher will require the certificate to no be self signed to prevent MITM
		 * attacks.
		 */

#ifdef USE_SSL
		if (!EnableSSL)
		{
			/* execute the alter system statement to enable ssl on within postgres */
			Node *enableSSLParseTree = ParseTreeNode(ENABLE_SSL_QUERY);
			AlterSystemSetConfigFile((AlterSystemStmt *) enableSSLParseTree);

			/*
			 * ssl mode requires that a key and certificate are present, since we have
			 * enabled ssl mode here chances are the user didn't install any credentials.
			 * This function will check if they are available and if not it will generate
			 * self singed certificates
			 */
			CreateCertificatesWhenNeeded();

			/* changing ssl configuration requires a reload of the configuration */
			DirectFunctionCall0(pg_reload_conf);
		}
#endif /* USE_SSL */
	}
}


/*
 * ExtractNewExtensionVersion returns the new extension version specified by
 * a CREATE or ALTER EXTENSION statement. Other inputs are not permitted. This
 * function returns NULL for statements with no explicit version specified.
 */
static char *
ExtractNewExtensionVersion(Node *parsetree)
{
	char *newVersion = NULL;
	List *optionsList = NIL;
	ListCell *optionsCell = NULL;

	if (IsA(parsetree, CreateExtensionStmt))
	{
		optionsList = ((CreateExtensionStmt *) parsetree)->options;
	}
	else if (IsA(parsetree, AlterExtensionStmt))
	{
		optionsList = ((AlterExtensionStmt *) parsetree)->options;
	}
	else
	{
		/* input must be one of the two above types */
		Assert(false);
	}

	foreach(optionsCell, optionsList)
	{
		DefElem *defElement = (DefElem *) lfirst(optionsCell);
		if (strncmp(defElement->defname, "new_version", NAMEDATALEN) == 0)
		{
			newVersion = strVal(defElement->arg);
			break;
		}
	}

	return newVersion;
}


static Datum
DirectFunctionCall0Coll(PGFunction func, Oid collation)
{
	FunctionCallInfoData fcinfo;
	Datum result;

	InitFunctionCallInfoData(fcinfo, NULL, 0, collation, NULL, NULL);

	result = (*func)(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
	{
		elog(ERROR, "function %p returned NULL", (void *) func);
	}

	return result;
}


#ifdef USE_SSL

/*
 * CreateCertificatesWhenNeeded checks if the certificates exists. When they don't exist
 * they will be created. The return value tells whether or not new certificates have been
 * created. After this function it is guaranteed that certificates are in place. It is not
 * guaranteed they have the right permissions as we will not touch the keys if they exist.
 */
static bool
CreateCertificatesWhenNeeded()
{
	EVP_PKEY *pkey = NULL;
	X509 *x509 = NULL;
	bool filesWritten = false;

	/*
	 * check if we can load the certificate, when we can we assume the certificates are im
	 * place
	 */
	if (SSL_CTX_use_certificate_chain_file(NULL, ssl_cert_file) == 1)
	{
		/* we didn't create any so return false */
		return false;
	}


	/* Generate the key. */
	pkey = generate_key();
	if (!pkey)
	{
		ereport(ERROR, (errmsg("error while generating private key")));
		return false;
	}

	/* Generate the certificate. */
	x509 = generate_x509(pkey);
	if (!x509)
	{
		EVP_PKEY_free(pkey);
		ereport(ERROR, (errmsg("error while generating x509 certificate")));
		return false;
	}

	/* Write the private key and certificate out to disk. */
	filesWritten = write_to_disk(pkey, x509);
	EVP_PKEY_free(pkey);
	X509_free(x509);

	if (!filesWritten)
	{
		ereport(ERROR, (errmsg("error while writing key and certificate to file")));
		return false;
	}

	return true;
}


/* Generates a 2048-bit RSA key. */
static EVP_PKEY *
generate_key()
{
	/* Allocate memory for the EVP_PKEY structure. */
	EVP_PKEY *pkey = EVP_PKEY_new();
	if (!pkey)
	{
		ereport(ERROR, (errmsg("unable to create EVP_PLEY")));
		return NULL;
	}

	/* Generate the RSA key and assign it to pkey. */
	RSA *rsa = RSA_generate_key(2048, RSA_F4, NULL, NULL);
	if (!EVP_PKEY_assign_RSA(pkey, rsa))
	{
		ereport(ERROR, (errmsg("unable to generate RSA key")));
		EVP_PKEY_free(pkey);
		return NULL;
	}

	/* The key has been generated, return it. */
	return pkey;
}


/* Generates a self-signed x509 certificate. */
static X509 *
generate_x509(EVP_PKEY *pkey)
{
	/* Allocate memory for the X509 structure. */
	X509 *x509 = X509_new();
	if (!x509)
	{
		ereport(ERROR, (errmsg("unable to create x509 cert")));
		return NULL;
	}

	/* Set the serial number. */
	ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);

	/* This certificate is valid from now until exactly one year from now. */
	X509_gmtime_adj(X509_get_notBefore(x509), 0);
	X509_gmtime_adj(X509_get_notAfter(x509), 31536000L);

	/* Set the public key for our certificate. */
	X509_set_pubkey(x509, pkey);

	/* We want to copy the subject name to the issuer name. */
	X509_NAME *name = X509_get_subject_name(x509);

	/* Set the country code and common name. */
	X509_NAME_add_entry_by_txt(name, X509_SUBJECT_COMMON_NAME, MBSTRING_ASC,
							   (unsigned char *) CITUS_AUTO_SSL_COMMON_NAME, -1, -1, 0);

	/* Now set the issuer name. */
	X509_set_issuer_name(x509, name);

	/* create self signed certificate. */
	if (!X509_sign(x509, pkey, EVP_sha256()))
	{
		X509_free(x509);
		ereport(ERROR, (errmsg("unable to sign x509 cert")));
		return NULL;
	}

	return x509;
}


static bool
write_to_disk(EVP_PKEY *pkey, X509 *x509)
{
	const char *keyFile = ssl_key_file;
	const char *certFile = ssl_cert_file;

	/* Open the PEM file for writing the key to disk. */
	FILE *pkey_file = fopen(keyFile, "wb");
	if (!pkey_file)
	{
		ereport(ERROR, (errmsg("unable to open key file '%s' for writing", keyFile)));
		return false;
	}

	/* Write the key to disk. */
	bool ret = PEM_write_PrivateKey(pkey_file, pkey, NULL, NULL, 0, NULL, NULL);
	fclose(pkey_file);

	if (!ret)
	{
		ereport(ERROR, (errmsg("unable to write private key to disk")));
		return false;
	}

	/* Open the PEM file for writing the certificate to disk. */
	FILE *x509_file = fopen(certFile, "wb");
	if (!x509_file)
	{
		ereport(ERROR, (errmsg("unable to open certificate file '%s' for writing",
							   certFile)));
		return false;
	}

	/* Write the certificate to disk. */
	ret = PEM_write_X509(x509_file, x509);
	fclose(x509_file);

	if (!ret)
	{
		ereport(ERROR, (errmsg("unable to write certificate to disk")));
		return false;
	}

	return true;
}


#endif /* USE_SSL */
