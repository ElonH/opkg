/* vi: set noexpandtab sw=4 sts=4: */
/* opkg_download.c - the opkg package management system

   Carl D. Worth

   Copyright (C) 2001 University of Southern California
   Copyright (C) 2008 OpenMoko Inc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>
#include <libgen.h>

#include "opkg_download.h"
#include "opkg_message.h"

#include "sprintf_alloc.h"
#include "xsystem.h"
#include "file_util.h"
#include "opkg_defines.h"
#include "libbb/libbb.h"

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#if defined(HAVE_SSLCURL) || defined(HAVE_OPENSSL)
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#if defined(HAVE_OPENSSL)
#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/hmac.h>
#endif

#if defined(HAVE_OPENSSL) || defined(HAVE_SSLCURL)
static void openssl_init(void);
#endif

#ifdef HAVE_OPENSSL
static X509_STORE *setup_verify(char *CAfile, char *CApath);
#endif

#ifdef HAVE_CURL
/*
 * Make curl an instance variable so we don't have to instanciate it
 * each time
 */
static CURL *curl = NULL;
static CURL *opkg_curl_init(curl_progress_func cb, void *data);
#endif

static int str_starts_with(const char *str, const char *prefix)
{
	return (strncmp(str, prefix, strlen(prefix)) == 0);
}

int
opkg_download(const char *src, const char *dest_file_name,
	      curl_progress_func cb, void *data, const short hide_error)
{
	int err = 0;

	char *src_basec = xstrdup(src);
	char *src_base = basename(src_basec);
	char *tmp_file_location;

	opkg_msg(NOTICE, "Downloading %s\n", src);

	if (str_starts_with(src, "file:")) {
		const char *file_src = src + 5;
		opkg_msg(INFO, "Copying %s to %s...", file_src, dest_file_name);
		err = file_copy(file_src, dest_file_name);
		opkg_msg(INFO, "Done.\n");
		free(src_basec);
		return err;
	}

	sprintf_alloc(&tmp_file_location, "%s/%s", conf->tmp_dir, src_base);
	free(src_basec);
	err = unlink(tmp_file_location);
	if (err && errno != ENOENT) {
		opkg_perror(ERROR, "Failed to unlink %s", tmp_file_location);
		free(tmp_file_location);
		return -1;
	}

	if (conf->http_proxy) {
		opkg_msg(DEBUG,
			 "Setting environment variable: http_proxy = %s.\n",
			 conf->http_proxy);
		setenv("http_proxy", conf->http_proxy, 1);
	}
	if (conf->ftp_proxy) {
		opkg_msg(DEBUG,
			 "Setting environment variable: ftp_proxy = %s.\n",
			 conf->ftp_proxy);
		setenv("ftp_proxy", conf->ftp_proxy, 1);
	}
	if (conf->no_proxy) {
		opkg_msg(DEBUG,
			 "Setting environment variable: no_proxy = %s.\n",
			 conf->no_proxy);
		setenv("no_proxy", conf->no_proxy, 1);
	}
#ifdef HAVE_CURL
	CURLcode res;
	FILE *file = fopen(tmp_file_location, "w");

	curl = opkg_curl_init(cb, data);
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, src);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

		res = curl_easy_perform(curl);
		fclose(file);
		if (res) {
			long error_code;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
					  &error_code);
			opkg_msg(hide_error ? DEBUG2 : ERROR,
				 "Failed to download %s: %s.\n", src,
				 curl_easy_strerror(res));
			free(tmp_file_location);
			return -1;
		}

	} else {
		free(tmp_file_location);
		return -1;
	}
#else
	{
		int res;
		const char *argv[8];
		int i = 0;

		argv[i++] = "wget";
		argv[i++] = "-q";
		if (conf->http_proxy || conf->ftp_proxy) {
			argv[i++] = "-Y";
			argv[i++] = "on";
		}
		argv[i++] = "-O";
		argv[i++] = tmp_file_location;
		argv[i++] = src;
		argv[i++] = NULL;
		res = xsystem(argv);

		if (res) {
			opkg_msg(ERROR,
				 "Failed to download %s, wget returned %d.\n",
				 src, res);
			if (res == 4)
				opkg_msg(ERROR,
					 "Check your network settings and connectivity.\n\n");
			free(tmp_file_location);
			return -1;
		}
	}
#endif

	err = file_move(tmp_file_location, dest_file_name);

	free(tmp_file_location);

	return err;
}

static int
opkg_download_cache(const char *src, const char *dest_file_name,
		    curl_progress_func cb, void *data)
{
	char *cache_name = xstrdup(src);
	char *cache_location, *p;
	int err = 0;

	if (!conf->cache || str_starts_with(src, "file:")) {
		err = opkg_download(src, dest_file_name, cb, data, 0);
		goto out1;
	}

	if (!file_is_dir(conf->cache)) {
		opkg_msg(ERROR, "%s is not a directory.\n", conf->cache);
		err = 1;
		goto out1;
	}

	for (p = cache_name; *p; p++)
		if (*p == '/')
			*p = ',';	/* looks nicer than | or # */

	sprintf_alloc(&cache_location, "%s/%s", conf->cache, cache_name);
	if (file_exists(cache_location))
		opkg_msg(NOTICE, "Copying %s.\n", cache_location);
	else {
		/* cache file with funky name not found, try simple name */
		free(cache_name);
		char *filename = strrchr(dest_file_name, '/');
		if (filename)
			cache_name = xstrdup(filename + 1);	// strip leading '/'
		else
			cache_name = xstrdup(dest_file_name);
		free(cache_location);
		sprintf_alloc(&cache_location, "%s/%s", conf->cache,
			      cache_name);
		if (file_exists(cache_location))
			opkg_msg(NOTICE, "Copying %s.\n", cache_location);
		else {
			err = opkg_download(src, cache_location, cb, data, 0);
			if (err) {
				(void)unlink(cache_location);
				goto out2;
			}
		}
	}

	err = file_copy(cache_location, dest_file_name);

out2:
	free(cache_location);
out1:
	free(cache_name);
	return err;
}

int opkg_download_pkg(pkg_t * pkg, const char *dir)
{
	int err;
	char *url;
	char *local_filename;
	char *stripped_filename;
	char *filename;

	if (pkg->src == NULL) {
		opkg_msg(ERROR,
			 "Package %s is not available from any configured src.\n",
			 pkg->name);
		return -1;
	}

	filename = pkg_get_string(pkg, PKG_FILENAME);

	if (filename == NULL) {
		opkg_msg(ERROR,
			 "Package %s does not have a valid filename field.\n",
			 pkg->name);
		return -1;
	}

	sprintf_alloc(&url, "%s/%s", pkg->src->value, filename);

	/* The filename might be something like
	   "../../foo.opk". While this is correct, and exactly what we
	   want to use to construct url above, here we actually need to
	   use just the filename part, without any directory. */

	stripped_filename = strrchr(filename, '/');
	if (!stripped_filename)
		stripped_filename = filename;

	sprintf_alloc(&local_filename, "%s/%s", dir, stripped_filename);
	pkg_set_string(pkg, PKG_LOCAL_FILENAME, local_filename);

	err = opkg_download_cache(url, local_filename, NULL, NULL);
	free(url);

	return err;
}

/*
 * Downloads file from url, installs in package database, return package name.
 */
int opkg_prepare_url_for_install(const char *url, char **namep)
{
	int err = 0;
	pkg_t *pkg;
	abstract_pkg_t *ab_pkg;

	pkg = pkg_new();

	if (str_starts_with(url, "http://")
	    || str_starts_with(url, "ftp://")) {
		char *tmp_file;
		char *file_basec = xstrdup(url);
		char *file_base = basename(file_basec);

		sprintf_alloc(&tmp_file, "%s/%s", conf->tmp_dir, file_base);
		err = opkg_download(url, tmp_file, NULL, NULL, 0);
		if (err)
			return err;

		err = pkg_init_from_file(pkg, tmp_file);
		if (err)
			return err;

		free(tmp_file);
		free(file_basec);

	} else if (strcmp(&url[strlen(url) - 4], OPKG_PKG_EXTENSION) == 0
		   || strcmp(&url[strlen(url) - 4], IPKG_PKG_EXTENSION) == 0
		   || strcmp(&url[strlen(url) - 4], DPKG_PKG_EXTENSION) == 0) {

		err = pkg_init_from_file(pkg, url);
		if (err)
			return err;
		opkg_msg(DEBUG2, "Package %s provided by hand (%s).\n",
			 pkg->name, pkg_get_string(pkg, PKG_LOCAL_FILENAME));
		pkg->provided_by_hand = 1;

	} else {
		ab_pkg = ensure_abstract_pkg_by_name(url);

		if (!(ab_pkg->state_flag & SF_NEED_DETAIL)) {
			opkg_msg(DEBUG, "applying abpkg flag to %s\n", ab_pkg->name);
			ab_pkg->state_flag |= SF_NEED_DETAIL;
		}

		pkg_deinit(pkg);
		free(pkg);
		return 0;
	}

	pkg->dest = conf->default_dest;
	pkg->state_want = SW_INSTALL;
	pkg->state_flag |= SF_PREFER;
	hash_insert_pkg(pkg, 1);

	if (namep) {
		*namep = xstrdup(pkg->name);
	}
	return 0;
}

int opkg_verify_file(char *text_file, char *sig_file)
{
#if defined HAVE_USIGN
	int status = -1;
	int pid;

	if (conf->check_signature == 0)
		return 0;

	pid = fork();
	if (pid < 0)
		return -1;

	if (!pid) {
		execl("/usr/sbin/opkg-key", "opkg-key", "verify", sig_file,
		      text_file, NULL);
		exit(255);
	}

	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		return -1;

	return 0;
#elif defined HAVE_OPENSSL
	X509_STORE *store = NULL;
	PKCS7 *p7 = NULL;
	BIO *in = NULL, *indata = NULL;

	// Sig check failed by default !
	int status = -1;

	openssl_init();

	// Set-up the key store
	if (!
	    (store =
	     setup_verify(conf->signature_ca_file, conf->signature_ca_path))) {
		opkg_msg(ERROR, "Can't open CA certificates.\n");
		goto verify_file_end;
	}
	// Open a BIO to read the sig file
	if (!(in = BIO_new_file(sig_file, "rb"))) {
		opkg_msg(ERROR, "Can't open signature file %s.\n", sig_file);
		goto verify_file_end;
	}
	// Read the PKCS7 block contained in the sig file
	p7 = PEM_read_bio_PKCS7(in, NULL, NULL, NULL);
	if (!p7) {
		opkg_msg(ERROR, "Can't read signature file %s (Corrupted ?).\n",
			 sig_file);
		goto verify_file_end;
	}

	// Open the Package file to authenticate
	if (!(indata = BIO_new_file(text_file, "rb"))) {
		opkg_msg(ERROR, "Can't open file %s.\n", text_file);
		goto verify_file_end;
	}
	// Let's verify the autenticity !
	if (PKCS7_verify(p7, NULL, store, indata, NULL, PKCS7_BINARY) != 1) {
		// Get Off My Lawn!
		opkg_msg(ERROR, "Verification failure.\n");
	} else {
		// Victory !
		status = 0;
	}

verify_file_end:
	BIO_free(in);
	BIO_free(indata);
	PKCS7_free(p7);
	X509_STORE_free(store);

	return status;
#else
	/* mute `unused variable' warnings. */
	(void)sig_file;
	(void)text_file;
	(void)conf;
	return 0;
#endif
}

#if defined(HAVE_OPENSSL) || defined(HAVE_SSLCURL)
static void openssl_init(void)
{
	static int init = 0;

	if (!init) {
		OPENSSL_config(NULL);
		OpenSSL_add_all_algorithms();
		ERR_load_crypto_strings();
		init = 1;
	}
}

#endif

#if defined HAVE_OPENSSL
static X509_STORE *setup_verify(char *CAfile, char *CApath)
{
	X509_STORE *store = NULL;
	X509_LOOKUP *lookup = NULL;

	if (!(store = X509_STORE_new())) {
		// Something bad is happening...
		goto end;
	}
	// adds the X509 file lookup method
	lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
	if (lookup == NULL) {
		goto end;
	}
	// Autenticating against one CA file
	if (CAfile) {
		if (!X509_LOOKUP_load_file(lookup, CAfile, X509_FILETYPE_PEM)) {
			// Invalid CA => Bye bye
			opkg_msg(ERROR, "Error loading file %s.\n", CAfile);
			goto end;
		}
	} else {
		X509_LOOKUP_load_file(lookup, NULL, X509_FILETYPE_DEFAULT);
	}

	// Now look into CApath directory if supplied
	lookup = X509_STORE_add_lookup(store, X509_LOOKUP_hash_dir());
	if (lookup == NULL) {
		goto end;
	}

	if (CApath) {
		if (!X509_LOOKUP_add_dir(lookup, CApath, X509_FILETYPE_PEM)) {
			opkg_msg(ERROR, "Error loading directory %s.\n",
				 CApath);
			goto end;
		}
	} else {
		X509_LOOKUP_add_dir(lookup, NULL, X509_FILETYPE_DEFAULT);
	}

	// All right !
	ERR_clear_error();
	return store;

end:

	X509_STORE_free(store);
	return NULL;

}

#endif

#ifdef HAVE_CURL
void opkg_curl_cleanup(void)
{
	if (curl != NULL) {
		curl_easy_cleanup(curl);
		curl = NULL;
	}
}

static CURL *opkg_curl_init(curl_progress_func cb, void *data)
{

	if (curl == NULL) {
		curl = curl_easy_init();

#ifdef HAVE_SSLCURL
		openssl_init();

		if (conf->ssl_engine) {

			/* use crypto engine */
			if (curl_easy_setopt
			    (curl, CURLOPT_SSLENGINE,
			     conf->ssl_engine) != CURLE_OK) {
				opkg_msg(ERROR,
					 "Can't set crypto engine '%s'.\n",
					 conf->ssl_engine);

				opkg_curl_cleanup();
				return NULL;
			}
			/* set the crypto engine as default */
			if (curl_easy_setopt
			    (curl, CURLOPT_SSLENGINE_DEFAULT, 1L) != CURLE_OK) {
				opkg_msg(ERROR,
					 "Can't set crypto engine '%s' as default.\n",
					 conf->ssl_engine);

				opkg_curl_cleanup();
				return NULL;
			}
		}

		/* cert & key can only be in PEM case in the same file */
		if (conf->ssl_key_passwd) {
			if (curl_easy_setopt
			    (curl, CURLOPT_SSLKEYPASSWD,
			     conf->ssl_key_passwd) != CURLE_OK) {
				opkg_msg(DEBUG,
					 "Failed to set key password.\n");
			}
		}

		/* sets the client certificate and its type */
		if (conf->ssl_cert_type) {
			if (curl_easy_setopt
			    (curl, CURLOPT_SSLCERTTYPE,
			     conf->ssl_cert_type) != CURLE_OK) {
				opkg_msg(DEBUG,
					 "Failed to set certificate format.\n");
			}
		}
		/* SSL cert name isn't mandatory */
		if (conf->ssl_cert) {
			curl_easy_setopt(curl, CURLOPT_SSLCERT, conf->ssl_cert);
		}

		/* sets the client key and its type */
		if (conf->ssl_key_type) {
			if (curl_easy_setopt
			    (curl, CURLOPT_SSLKEYTYPE,
			     conf->ssl_key_type) != CURLE_OK) {
				opkg_msg(DEBUG, "Failed to set key format.\n");
			}
		}
		if (conf->ssl_key) {
			if (curl_easy_setopt
			    (curl, CURLOPT_SSLKEY, conf->ssl_key) != CURLE_OK) {
				opkg_msg(DEBUG, "Failed to set key.\n");
			}
		}

		/* Should we verify the peer certificate ? */
		if (conf->ssl_dont_verify_peer) {
			/*
			 * CURLOPT_SSL_VERIFYPEER default is nonzero (curl => 7.10)
			 */
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
		}

		/* certification authority file and/or path */
		if (conf->ssl_ca_file) {
			curl_easy_setopt(curl, CURLOPT_CAINFO,
					 conf->ssl_ca_file);
		}
		if (conf->ssl_ca_path) {
			curl_easy_setopt(curl, CURLOPT_CAPATH,
					 conf->ssl_ca_path);
		}
#endif

		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
		curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
		if (conf->http_proxy || conf->ftp_proxy) {
			char *userpwd;
			sprintf_alloc(&userpwd, "%s:%s", conf->proxy_user,
				      conf->proxy_passwd);
			curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, userpwd);
			free(userpwd);
		}
	}

	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, (cb == NULL));
	if (cb) {
		curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, data);
		curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, cb);
	}

	return curl;

}
#endif
