#define _GNU_SOURCE
/*
 * Copyright 2007, Intel Corporation
 *
 * This file is part of corewatcher.org
 *
 * This program file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program in a file named COPYING; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 *
 * Authors:
 *	Arjan van de Ven <arjan@linux.intel.com>
 *	William Douglas <william.douglas@intel.com>
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <signal.h>
#include <glib.h>
#include <errno.h>

#include "corewatcher.h"

int uid = 0;
int sig = 0;

const char *core_folder = "/var/lib/corewatcher/";
const char *processed_folder = "/var/lib/corewatcher/processed/";

/*
 * the application must initialize the GMutex's
 * core_status.processing_mtx, core_status.queued_mtx,
 * processing_queue_mtx and gdb_mtx
 * before calling into this file's scan_corefolders()
 * (also since that calls submit_queue() there are dependencies
 *  there which need taken care of too)
 */
/* Always pick up the processing_mtx and then the
   processing_queue_mtx, reverse for setting down */
/* Always pick up the gdb_mtx and then the
   processing_queue_mtx, reverse for setting down */
/* Always pick up the processing_mtx and then the
   gdb_mtx, reverse for setting down */
/* so order for pick up should be:
   processing_mtx -> gdb_mtx -> processing_queue_mtx
   and the reverse for setting down */
GMutex processing_queue_mtx;
static char *processing_queue[MAX_PROCESSING_OOPS];
static int pq_tail = 0;
static int pq_head = 0;
GMutex gdb_mtx;

static char *get_release(void)
{
	FILE *file = NULL;
	char *line = NULL;
	size_t dummy = 0;

	file = fopen("/etc/os-release", "r");
	if (!file) {
		line = strdup("Unknown");
		return line;
	}

	while (!feof(file)) {
		if (getline(&line, &dummy, file) == -1)
			break;
		if (strstr(line, "VERSION_ID=")) {
			char *c = NULL;

			c = strchr(line, '\n');
			if (c) {
				*c = 0;
				c = strdup(&line[11]);
				fclose(file);
				free(line);
				return c;
			}
		}
	}

	fclose(file);
	free(line);

	line = strdup("Unknown");

	return line;
}

static char *set_wrapped_app(char *line)
{
	char *dline = NULL, *app = NULL, *appfile = NULL, *abs_path = NULL;
	char delim[] = " '";
	char app_folder[] = "/usr/share/";
	int r = 0;

	if (!line)
		return NULL;

	dline = strdup(line);

	app = strtok(dline, delim);
	while(app) {
		if (strcmp(app, "--app") == 0) {
			app = strtok(NULL, delim);
			break;
		}
		app = strtok(NULL, delim);
	}
	if (!app)
		goto cleanup_set_wrapped_app;
	r = asprintf(&abs_path, "%s%s", app_folder, app);
	if (r == -1) {
		abs_path = NULL;
		goto cleanup_set_wrapped_app;
	} else if (((unsigned int)r) != strlen(app_folder) + strlen(app)) {
		goto cleanup_set_wrapped_app;
	}

	appfile = find_executable(abs_path);

cleanup_set_wrapped_app:
	free(abs_path);
	free(dline);
	return appfile;
}

/*
 * Scan core dump in case a wrapper was used
 * to run the process and get the actual binary name
 */
static char *wrapper_scan(char *command)
{
	char *line = NULL, *appfile = NULL;
	FILE *file = NULL;

	file = popen(command, "r");
	if (!file)
		return NULL;

	while (!feof(file)) {
		size_t size = 0;
		int ret = 0;
		free(line);
		ret = getline(&line, &size, file);
		if (!size)
			break;
		if (ret < 0)
			break;

		if (strstr(line, "Core was generated by") &&
		    strstr(line, "--app")) {
			/* attempt to update appfile */
			appfile = set_wrapped_app(line);
			break;
		}
	}
	if (line)
		free(line);
	pclose(file);

	return appfile;
}

/*
 * Strip the directories from the path
 * given by fullname
 */
char *strip_directories(char *fullpath)
{
	char *dfile = NULL, *c1 = NULL, *c2 = NULL, *r = NULL;
	char delim[] = "/";

	if (!fullpath)
		return NULL;

	dfile = strdup(fullpath);
	if (!dfile)
		return NULL;

	c1 = strtok(dfile, delim);
	while(c1) {
		c2 = c1;
		c1 = strtok(NULL, delim);
	}

	if (c2)
		r = strdup(c2);
	free(dfile);

	return r;
}

/*
 * Move corefile from core_folder to processed_folder subdir.
 * If this type of core has recently been seen, unlink this more recent
 * example in order to rate limit submissions of extremely crashy
 * applications.
 * Add extension if given and attempt to create directories if needed.
 */
int move_core(char *fullpath, char *extension)
{
	char *corefilename = NULL, newpath[8192], *coreprefix = NULL;
	char *s = NULL;
	size_t prefix_len;
	DIR *dir = NULL;
	struct dirent *entry = NULL;

	if (!fullpath)
		return -1;

	if (!(corefilename = strip_directories(fullpath)))
		return -ENOMEM;

	/* if the corefile's name minus any suffixes (such as .$PID) and
	 * minus two additional characters (ie: last two digits of
	 * timestamp assuming core_%e_%t) matches another core file in the
	 * processed_folder, simply unlink it instead of processing it for
	 * submission.  TODO: consider a (configurable) time delta greater
	 * than which the cores must be separated, stat'ing the files, etc.
	 */
	if (!(coreprefix = strdup(corefilename)))
		return -ENOMEM;
	if (!(s = strstr(coreprefix, ".")))
		return -1;
	*s = '\0';
	prefix_len = strlen(coreprefix);
	if (prefix_len > 2) {
		s = strndup(coreprefix, prefix_len - 2);
		free(coreprefix);
		coreprefix = s;
	} else {
		goto error;
	}
	dir = opendir(processed_folder);
	if (!dir)
		goto error;
	while(1) {
		entry = readdir(dir);
		if (!entry || !entry->d_name)
			break;
		if (entry->d_name[0] == '.')
			continue;
		if (!strstr(entry->d_name, coreprefix))
			continue;
		fprintf(stderr, "+ ...ignoring/unlinking %s\n", fullpath);
		unlink(fullpath);
		goto error;
	}

	if (extension)
		snprintf(newpath, 8192, "%s%s.%s", processed_folder, corefilename, extension);
	else
		snprintf(newpath, 8192, "%s%s", processed_folder, corefilename);

	free(coreprefix);
	free(corefilename);
	rename(fullpath, newpath);

	return 0;
error:
	free(coreprefix);
	free(corefilename);
	return -1;
}

/*
 * Finds the full path for the application that crashed,
 * and moves file to processed_folder for processing
 */
static char *get_appfile(char *fullpath)
{
	char *appname = NULL, *appfile = NULL;

	if (!fullpath)
		return NULL;

	appname = find_coredump(fullpath);
	if (!appname)
		return NULL;

	/* don't try and do anything for rpm, gdb or corewatcher crashes */
	if (!(strcmp(appname, "rpm") && strcmp(appname, "gdb") && strcmp(appname, "corewatcher")))
		return NULL;

	appfile = find_executable(appname);
	/* appname no longer used, so free it as it was strdup'd */
	free(appname);
	if (!appfile)
		return NULL;

	move_core(fullpath, "to-process");

	return appfile;
}

/*
 * Use GDB to extract backtrace information from corefile
 */
static struct oops *extract_core(char *fullpath, char *appfile)
{
	struct oops *oops = NULL;
	int ret = 0;
	char *command = NULL, *h1 = NULL, *c1 = NULL, *c2 = NULL, *line = NULL;
	char *text = NULL, *at = NULL, *coretime = NULL;
	char *m1 = NULL, *m2 = NULL;
	FILE *file = NULL;
	char *badchar = NULL;
	char *release = get_release();
	int parsing_maps = 0;
	struct stat stat_buf;

	if (asprintf(&command, "LANG=C gdb --batch -f %s %s -x /etc/corewatcher/gdb.command 2> /dev/null", appfile, fullpath) == -1)
		return NULL;

	if (stat(fullpath, &stat_buf) != -1) {
		coretime = ctime(&stat_buf.st_mtime);
	}
	if (coretime == NULL) {
		if (asprintf(&coretime, "Unknown\n") == -1)
			return NULL;
	}

	if ((at = wrapper_scan(command))) {
		free(appfile);
		appfile = at;
	}

	ret = asprintf(&h1,
		       "cmdline: %s\n"
		       "release: %s\n"
		       "time: %s",
		       appfile,
		       release,
		       coretime);
	free(release);
	if (ret == -1)
		h1 = strdup("Unknown");

	file = popen(command, "r");

	while (file && !feof(file)) {
		size_t size = 0;

		free(line);
		line = NULL;
		ret = getline(&line, &size, file);
		if (!size)
			break;
		if (ret == -1)
			break;

		if (strncmp(line, "From", 4) == 0) {
			parsing_maps = 1;
			/*continue;*/
		}

		if (!parsing_maps) { /* parsing backtrace */
			c2 = c1;
			if (line[0] != '#')
				continue;
fixup:			/* gdb outputs some 0x1a's which break XML */
			badchar = memchr(line, 0x1a, size);
			if (badchar) {
				*badchar = ' ';
				goto fixup;
			}

			if (c1) {
				c1 = NULL;
				if (asprintf(&c1, "%s        %s", c2, line) == -1)
					continue;
				free(c2);
			} else {
				/* keep going even if asprintf has errors */
				ret = asprintf(&c1, "        %s", line);
			}
		} else { /* parsing maps */
			m2 = m1;
			if (m1) {
				m1 = NULL;
				if (asprintf(&m1, "%s        %s", m2, line) == -1)
					continue;
				free(m2);
			} else {
				/* keep going even if asprintf has errors */
				ret = asprintf(&m1, "        %s", line);
			}
		}
	}
	if (line)
		free(line);
	if (file)
		pclose(file);
	free(command);

	ret = asprintf(&text,
		       "%s"
		       "backtrace: |\n"
		       "%s"
		       "maps: |\n"
		       "%s",
		       h1, c1, m1);
	if (ret == -1)
		text = NULL;

	free(h1);
	free(c1);

	oops = malloc(sizeof(struct oops));
	if (!oops) {
		free(text);
		return NULL;
	}
	memset(oops, 0, sizeof(struct oops));
	oops->application = strdup(appfile);
	oops->text = text;
	oops->filename = strdup(fullpath);
	return oops;
}

/*
 * filename is of the form core_XXXX[.blah]
 * we need to get the pid out as we want
 * output of the form XXXX[.ext]
 */
char *get_core_filename(char *filename, char *ext)
{
	char *pid = NULL, *c = NULL, *s = NULL, *detail_filename = NULL;

	if (!filename)
		return NULL;

	if (!(s = strstr(filename, "_")))
		return NULL;

	if (!(++s))
		return NULL;
	/* causes valgrind whining because we copy from middle of a string */
	if (!(pid = strdup(s)))
		return NULL;

	c = strstr(pid, ".");

	if (c)
		*c = '\0';

	if (ext) {
		/* causes valgrind whining because we copy from middle of a string */
		if ((asprintf(&detail_filename, "%s%s.%s", processed_folder, pid, ext)) == -1) {
			free(pid);
			return NULL;
		}
	} else {
		/* causes valgrind whining because we copy from middle of a string */
		if ((asprintf(&detail_filename, "%s%s", processed_folder, pid)) == -1) {
			free(pid);
			return NULL;
		}
	}

	free(pid);
	return detail_filename;
}

/*
 * Write the backtrace from the core file into a text
 * file named after the pid
 */
static void write_core_detail_file(char *filename, char *text)
{
	int fd = 0;
	char *detail_filename = NULL;

	if (!filename || !text)
		return;

	if (!(detail_filename = get_core_filename(filename, "txt")))
		return;

	if ((fd = open(detail_filename, O_WRONLY | O_CREAT | O_TRUNC, 0)) != -1) {
		if(write(fd, text, strlen(text)) >= 0) {
			fchmod(fd, 0644);
		} else {
			fprintf(stderr, "+ ...ignoring/unlinking %s\n", detail_filename);
			unlink(detail_filename);
		}
		close(fd);
	}
	free(detail_filename);
}

/*
 * Removes corefile (core.XXXX) from the processing_queue.
 *
 * Expects the processing_queue_mtx to be held.
 */
static void remove_from_processing_queue(void)
{
	free(processing_queue[pq_head]);
	processing_queue[pq_head++] = NULL;

	if (pq_head == MAX_PROCESSING_OOPS)
		pq_head = 0;
}

/*
 * Removes file from processing_oops hash based on pid.
 * Extracts pid from the fullpath such that
 * /home/user/core.pid will be tranformed into just the pid.
 *
 * Expects the lock on the given hash to be held.
 */
void remove_pid_from_hash(char *fullpath, GHashTable *ht)
{
	char *c1 = NULL, *c2 = NULL;

	if (!(c1 = strip_directories(fullpath)))
		return;

	if (!(c2 = get_core_filename(c1, NULL))) {
		free(c1);
		return;
	}

	free(c1);

	g_hash_table_remove(ht, c2);

	free(c2);
}

/*
 * Common function for processing core
 * files to generate oops structures
 */
static struct oops *process_common(char *fullpath)
{
	struct oops *oops = NULL;
	char *appname = NULL, *appfile = NULL;

	if(!(appname = find_coredump(fullpath))) {
		return NULL;
	}

	if (!(appfile = find_executable(appname))) {
		free(appname);
		return NULL;
	}
	free(appname);

	if (!(oops = extract_core(fullpath, appfile))) {
		free(appfile);
		return NULL;
	}
	free(appfile);

	return oops;
}


/*
 * Processes .to-process core files.
 * Also creates the pid.txt file and adds
 * the oops struct to the submit queue
 *
 * Picks up and sets down the gdb_mtx and
 * picks up and sets down the processing_queue_mtx.
 * (held at the same time in that order)
 * Also will pick up and sets down the queued_mtx.
 */
static void *process_new(void __unused *vp)
{
	struct oops *oops = NULL;
	char *procfn = NULL, *corefn = NULL, *fullpath = NULL;

	g_mutex_lock(&core_status.processing_mtx);
	g_mutex_lock(&gdb_mtx);
	g_mutex_lock(&processing_queue_mtx);

	if (!(fullpath = processing_queue[pq_head])) {
		/* something went quite wrong */
		g_mutex_unlock(&processing_queue_mtx);
		g_mutex_unlock(&gdb_mtx);
		g_mutex_unlock(&core_status.processing_mtx);
		return NULL;
	}

	if (!(corefn = strip_directories(fullpath)))
		goto clean_process_new;

	if (!(procfn = replace_name(fullpath, ".to-process", ".processed")))
		goto clean_process_new;

	if (!(oops = process_common(fullpath)))
		goto clean_process_new;

	if (!(oops->detail_filename = get_core_filename(corefn, "txt")))
		goto clean_process_new;

	if (rename(fullpath, procfn))
		goto clean_process_new;

	free(oops->filename);
	oops->filename = procfn;

	remove_from_processing_queue();

	g_mutex_unlock(&processing_queue_mtx);
	g_mutex_unlock(&gdb_mtx);
	g_mutex_unlock(&core_status.processing_mtx);

	write_core_detail_file(corefn, oops->text);

	g_mutex_lock(&core_status.queued_mtx);
	queue_backtrace(oops);
	g_mutex_unlock(&core_status.queued_mtx);

	/* don't need to free procfn because was set to oops->filename and that gets free'd */
	free(corefn);
	FREE_OOPS(oops);
	return NULL;

clean_process_new:
	remove_pid_from_hash(fullpath, core_status.processing_oops);
	remove_from_processing_queue();
	free(procfn);
	procfn = NULL; /* don't know if oops->filename == procfn so be safe */
	free(corefn);
	FREE_OOPS(oops);
	g_mutex_unlock(&processing_queue_mtx);
	g_mutex_unlock(&gdb_mtx);
	g_mutex_unlock(&core_status.processing_mtx);
	return NULL;
}

/*
 * Reprocesses .processed core files.
 *
 * Picks up and sets down the gdb_mtx.
 * Picks up and sets down the processing_queue_mtx.
 * (held at the same time in that order)
 * Also will pick up and sets down the queued_mtx.
 */
static void *process_old(void __unused *vp)
{
	struct oops *oops = NULL;
	char *corefn = NULL, *fullpath = NULL;

	g_mutex_lock(&gdb_mtx);
	g_mutex_lock(&processing_queue_mtx);

	if (!(fullpath = processing_queue[pq_head])) {
		/* something went quite wrong */
		g_mutex_unlock(&processing_queue_mtx);
		g_mutex_unlock(&gdb_mtx);
		return NULL;
	}

	if (!(corefn = strip_directories(fullpath)))
		goto clean_process_old;

	if (!(oops = process_common(fullpath)))
		goto clean_process_old;

	if (!(oops->detail_filename = get_core_filename(corefn, "txt")))
		goto clean_process_old;

	remove_from_processing_queue();

	g_mutex_unlock(&processing_queue_mtx);
	g_mutex_unlock(&gdb_mtx);

	g_mutex_lock(&core_status.queued_mtx);
	queue_backtrace(oops);
	g_mutex_unlock(&core_status.queued_mtx);

	free(corefn);
	FREE_OOPS(oops);
	return NULL;

clean_process_old:
	remove_pid_from_hash(fullpath, core_status.processing_oops);
	remove_from_processing_queue();
	free(corefn);
	FREE_OOPS(oops);
	g_mutex_unlock(&processing_queue_mtx);
	g_mutex_unlock(&gdb_mtx);
	return NULL;
}

/*
 * Adds corefile (based on pid) to the processing_oops
 * hash table if it is not already there, then
 * tries to add to the processing_queue.
 *
 * Picks up and sets down the processing_mtx.
 * Picks up and sets down the processing_queue_mtx.
 */
static int add_to_processing(char *fullpath)
{
	char *c1 = NULL, *c2 = NULL, *fp = NULL;

	if (!fullpath)
		return -1;

	if (!(fp = strdup(fullpath)))
		goto clean_add_to_processing;

	if (!(c1 = get_core_filename(fp, NULL)))
		goto clean_add_to_processing;

	if (!(c2 = strip_directories(c1)))
		goto clean_add_to_processing;

	free(c1);
	c1 = NULL;

	g_mutex_lock(&core_status.processing_mtx);
	if (g_hash_table_lookup(core_status.processing_oops, c2)) {
		g_mutex_unlock(&core_status.processing_mtx);
		goto clean_add_to_processing;
	}

	g_mutex_lock(&processing_queue_mtx);
	if (processing_queue[pq_tail]) {
		g_mutex_unlock(&processing_queue_mtx);
		g_mutex_unlock(&core_status.processing_mtx);
		goto clean_add_to_processing;
	}

	g_hash_table_insert(core_status.processing_oops, c2, c2);
	processing_queue[pq_tail++] = fp;
	if (pq_tail == MAX_PROCESSING_OOPS)
		pq_tail = 0;

	g_mutex_unlock(&processing_queue_mtx);
	g_mutex_unlock(&core_status.processing_mtx);
	return 0;
clean_add_to_processing:
	free(fp);
	free(c1);
	free(c2);
	return -1;
}

/*
 * Entry for processing new core files.
 */
static void process_corefile(char *fullpath)
{
	GThread *thrd = NULL;
	int r = 1;

	r = add_to_processing(fullpath);

	if (r)
		return;

	thrd = g_thread_new("process_new", process_new, NULL);
	if (thrd == NULL)
		fprintf(stderr, "Couldn't start thread for process_new()\n");
}

/*
 * Entry for processing already seen core files.
 */
static void reprocess_corefile(char *fullpath)
{
	GThread *thrd = NULL;
	int r = 0;

	r = add_to_processing(fullpath);

	if (r)
		return;

	thrd = g_thread_new("process_old", process_old, NULL);
	if (thrd == NULL)
		fprintf(stderr, "Couldn't start thread for process_old()\n");
}

static void scan_core_folder(void __unused *unused)
{
	/* scan for new crash data */
	DIR *dir = NULL;
	struct dirent *entry = NULL;
	char *fullpath = NULL, *appfile = NULL;
	int r = 0;

	dir = opendir(core_folder);
	if (!dir)
		return;
	fprintf(stderr, "+ scanning %s...\n", core_folder);
	while(1) {
		free(fullpath);
		fullpath = NULL;

		entry = readdir(dir);
		if (!entry || !entry->d_name)
			break;
		if (entry->d_name[0] == '.')
			continue;
		if (strncmp(entry->d_name, "core_", 5))
			continue;

		/* matched core_#### where #### is the pid of the process */
		r = asprintf(&fullpath, "%s%s", core_folder, entry->d_name);
		if (r == -1) {
			fullpath = NULL;
			continue;
		} else if (((unsigned int)r) != strlen(core_folder) + strlen(entry->d_name)) {
			continue;
		}

		/* If one were to prompt the user before submitting, that
		 * might happen here.  */

		fprintf(stderr, "+ Looking at %s\n", fullpath);
		appfile = get_appfile(fullpath);

		if (!appfile) {
			fprintf(stderr, "+ ...ignoring/unlinking %s\n", fullpath);
			unlink(fullpath);
		} else {
			free(appfile);
			appfile = NULL;
		}
	}
	closedir(dir);
}

static void scan_processed_folder(void __unused *unused)
{
	/* scan for partially processed crash data */
	DIR *dir = NULL;
	struct dirent *entry = NULL;
	char *fullpath = NULL;
	int r = 0;

	dir = opendir(processed_folder);
	if (!dir)
		return;
	fprintf(stderr, "+ scanning %s...\n", processed_folder);
	while(1) {
		free(fullpath);
		fullpath = NULL;

		entry = readdir(dir);
		if (!entry || !entry->d_name)
			break;
		if (entry->d_name[0] == '.')
			continue;
		if (!strstr(entry->d_name, "process"))
			continue;

		r = asprintf(&fullpath, "%s%s", processed_folder, entry->d_name);
		if (r == -1) {
			fullpath = NULL;
			continue;
		} else if (((unsigned int)r) != strlen(processed_folder) + strlen(entry->d_name)) {
			continue;
		}

		fprintf(stderr, "+ Looking at %s\n", fullpath);
		if (strstr(fullpath, "to-process"))
			process_corefile(fullpath);
		else
			reprocess_corefile(fullpath);
	}
	closedir(dir);
}

int scan_corefolders(void __unused *unused)
{
	scan_core_folder(NULL);
	scan_processed_folder(NULL);

	submit_queue();

	return 1;
}
