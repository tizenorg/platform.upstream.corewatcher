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
#include <errno.h>

#include "corewatcher.h"

int do_unlink = 0;
int uid;
int sig;

#define MAX(A,B) ((A) > (B) ? (A) : (B))

long int get_time(char *filename)
{
	struct stat st;
	if (stat(filename, &st)) {
		return 0;
	}
	return st.st_mtim.tv_sec;
}

char *get_build(void) {
	FILE *file;
	char *line = NULL;
	size_t dummy;

	file = fopen(build_release, "r");
	if (!file) {
		line = strdup("Unknown");
		return line;
	}
	while (!feof(file)) {
		line = NULL;
		if (getline(&line, &dummy, file) <= 0)
			break;
		if (strstr(line, "BUILD") != NULL) {
			char *c, *build;

			c = strstr(line, "BUILD");
			c += 7;
			build = strdup(c);

			c = strchr(build, '\n');
			if (c) *c = 0;

			free(line);
			fclose(file);
			return build;
		} else {
			free(line);
		}
	}

	fclose(file);

	line = strdup("Unknown");

	return line;
}

static char *get_release(void) {
	FILE *file;
	char *line = NULL;
	size_t dummy;

	file = fopen("/etc/issue", "r");
	if (!file) {
		line = strdup("Unknown");
		return line;
	}

	while (!feof(file)) {
		line = NULL;
		if (getline(&line, &dummy, file) <= 0)
			break;

		if (strstr(line, "release") != NULL) {
			char *c;

			c = strchr(line, '\n');
			if (c) *c = 0;

			fclose(file);
			return line;
		}
	}

	fclose(file);

	line = strdup("Unknown");

	return line;
}

void set_wrapped_app(char *line)
{
	char *dline = NULL, *app = NULL, *ret = NULL, *abs_path = NULL;
	char delim[] = " '";

	dline = strdup(line);

	app = strtok(dline, delim);
	while(app) {
		if (strcmp(app, "--app") == 0) {
			app = strtok(NULL, delim);
			break;
		}
		app = strtok(NULL, delim);
	}
	if (asprintf(&abs_path, "/usr/share/%s", app) < 0) {
		free(dline);
		return;
	}
	ret = find_executable(abs_path);
	free(dline);
	/*
	 * May have got NULL from find_executable if app
	 * isn't in /usr/share but that's okay as we
	 * don't change the filename in that case.
	 */
}

char *signame(int sig)
{
	switch(sig) {
	case SIGINT:  return "SIGINT";
	case SIGILL:  return "SIGILL";
	case SIGABRT: return "SIGABRT";
	case SIGFPE:  return "SIGFPE";
	case SIGSEGV: return "SIGSEGV";
	case SIGPIPE: return "SIGPIPE";
	case SIGBUS:  return "SIGBUS";
	default:      return strsignal(sig);
	}
	return NULL;
}

static char *get_kernel(void) {
	char *command = NULL, *line = NULL;
	FILE *file;
	int ret = 0;
	size_t size = 0;

	if (asprintf(&command, "uname -r") < 0) {
		line = strdup("Unknown");
		free(command);
		return line;
	}

	file = popen(command, "r");
	free(command);

	ret = getline(&line, &size, file);
	if ((!size) || (ret < 0)) {
		line = strdup("Unknown");
		return line;
	}

	size = strlen(line);
	line[size - 1] = '\0';

	pclose(file);
	return line;
}

char *build_core_header(char *appfile, char *corefile) {
	int ret = 0;
	char *result = NULL;
	char *build = get_build();
	char *release = get_release();
	char *kernel = get_kernel();
	long int time = get_time(corefile);
	char *private = private_report ? "private: yes\n" : "";

	ret = asprintf(&result,
		       "analyzer: corewatcher-gdb\n"
		       "coredump: %s\n"
		       "executable: %s\n"
		       "kernel: %s\n"
		       "reason: Process %s was killed by signal %d (%s)\n"
		       "release: %s\n"
		       "build: %s\n"
		       "time: %lu\n"
		       "uid: %d\n"
	               "%s"
		       "\nbacktrace\n-----\n",
		       corefile,
		       appfile,
		       kernel,
		       appfile, sig, signame(sig),
		       release,
		       build,
		       time,
		       uid,
	               private);

	free(kernel);
	free(release);
	free(build);

	if (ret < 0)
		result = strdup("Unknown");

	return result;
}

/*
 * Scan core dump in case a wrapper was used
 * to run the process and get the actual binary name
 */
void wrapper_scan(char *command)
{
	char *line = NULL;
	FILE *file;

	file = popen(command, "r");
	while (!feof(file)) {
		size_t size = 0;
		int ret;
		free(line);
		ret = getline(&line, &size, file);
		if (!size)
			break;
		if (ret < 0)
			break;

		if (strstr(line, "Core was generated by") &&
		    strstr(line, "--app")) {
			/* attempt to update appfile */
			set_wrapped_app(line);
			break;
		}
	}
	if (line)
		free(line);
	pclose(file);
}

struct oops *extract_core(char *corefile)
{
	struct oops *oops;
	char *command = NULL, *c1 = NULL, *c2 = NULL, *line = NULL;
	char *coredump = NULL;
	char *appfile;
	FILE *file;

	coredump = find_coredump(corefile);
	if (!coredump)
		return NULL;

	appfile = find_executable(coredump);
	/* coredump no longer used, so free it as it was strdup'd */
	free(coredump);
	if (!appfile)
		return NULL;

	if (asprintf(&command, "LANG=C gdb --batch -f %s %s -x /var/lib/corewatcher/gdb.command 2> /dev/null", appfile, corefile) < 0) {
		free(command);
		return NULL;
	}

	wrapper_scan(command);
	c1 = build_core_header(appfile, corefile);

	file = popen(command, "r");
	while (!feof(file)) {
		size_t size = 0;
		int ret;

		c2 = c1;
		free(line);
		ret = getline(&line, &size, file);
		if (!size)
			break;
		if (ret < 0)
			break;

		if (strstr(line, "no debugging symbols found")) {
			continue;
		}
		if (strstr(line, "reason: ")) {
			continue;
		}

		if (c1) {
			c1 = NULL;
			if (asprintf(&c1, "%s%s", c2, line) < 0)
				continue;
			free(c2);
		} else {
			/* keep going even if asprintf has errors */
			asprintf(&c1, "%s", line);
		}
	}
	if (line)
		free(line);
	pclose(file);
	free(command);
	oops = malloc(sizeof(struct oops));
	if (!oops) {
		free(c1);
		return NULL;
	}
	memset(oops, 0, sizeof(struct oops));
	oops->application = strdup(appfile);
	oops->text = c1;
	oops->filename = strdup(corefile);
	return oops;
}

void write_core_detail_file(char *filename, char *text)
{
	int fd;
	char *pid;
	char detail_filename[8192];

	if (!(pid = strstr(filename, ".")))
		return;

	snprintf(detail_filename, 8192, "%s%s.txt", core_folder, ++pid);
	if ((fd = open(detail_filename, O_WRONLY | O_CREAT | O_TRUNC, 0)) != -1) {
		write(fd, text, strlen(text));
		fchmod(fd, 0644);
		close(fd);
	}
}

char *get_core_detail_filename(char *filename)
{
	char *pid, *c, *s;
	char *detail_filename;

	if (!(s = strstr(filename, ".")))
		return NULL;

	pid = strdup(++s);
	if (!(c = strstr(pid, "."))) {
		free(pid);
		return NULL;
	}

	*c = '\0';

	if ((asprintf(&detail_filename, "%s%s.txt", core_folder, pid)) < 0) {
		free(pid);
		return NULL;
	}

	free(pid);
	return detail_filename;
}

char *remove_directories(char *fullname)
{
	char *dfile = NULL, *c = NULL, *d = NULL;
	char delim[] = "/";

	dfile = strdup(fullname);
	if (!dfile)
		return NULL;

	c = strtok(dfile, delim);
	while(c) {
		d = c;
		c = strtok(NULL, delim);
	}
	d = strdup(d);
	free(dfile);

	return d;
}

void process_corefile(char *filename)
{
	struct oops *oops;
	char newfile[8192], *fn = NULL;

	oops = extract_core(filename);

	if (!oops) {
		unlink(filename);
		return;
	}

	if (!(fn = remove_directories(filename))) {
		FREE_OOPS(oops);
		return;
	}

	/* if this oops hasn't been processed before need to write details out and rename */
	if (!strstr(fn, ".processed")) {
		fprintf(stderr, "---[start of coredump]---\n%s\n---[end of coredump]---\n", oops->text);
		dbus_say_found(oops);

		if (!mkdir(core_folder, S_IRWXU | S_IRWXG | S_IRWXO) && errno != EEXIST) {
			free(fn);
			FREE_OOPS(oops);
			return;
		}
		/* try to write coredump text details to text file */
		write_core_detail_file(filename, oops->text);
		sprintf(newfile,"%s%s.processed", core_folder, fn);
		rename(filename, newfile);

	} else {
		/* backtrace queued only if files have been processed
		   to avoid putting a new coredump in twice */
		oops->detail_filename = get_core_detail_filename(fn);
		queue_backtrace(oops);
	}

	free(fn);
	FREE_OOPS(oops);
}


int scan_dmesg(void __unused *unused)
{
	DIR *dir;
	struct dirent *entry;
	char path[PATH_MAX*2];
	struct oops *oq;

	dir = opendir("/tmp/");
	if (!dir)
		return 1;

	fprintf(stderr, "+ scanning /tmp/...\n");
	do {
		entry = readdir(dir);
		if (!entry)
			break;
		if (entry->d_name[0] == '.')
			continue;
		if (strstr(entry->d_name, "processed"))
			continue;
		if (strncmp(entry->d_name, "core.", 5))
			continue;
		sprintf(path, "/tmp/%s", entry->d_name);
		fprintf(stderr, "+ Looking at %s\n", path);
		process_corefile(path);
	} while (entry);
	closedir(dir);

	dir = opendir(core_folder);
	if (!dir)
		return 1;

	fprintf(stderr, "+ scanning %s...\n", core_folder);
	do {
	skip:
		memset(path, 0, PATH_MAX*2);
		oq = get_oops_queue();
		entry = readdir(dir);
		if (!entry)
			break;
		if (!strstr(entry->d_name, ".processed"))
			continue;
		sprintf(path, "%s%s", core_folder, entry->d_name);
		while (oq) {
			if (!(strcmp(oq->filename, path)))
				goto skip;
			oq = oq->next;
		}
		fprintf(stderr, "+ Looking at %s\n", path);
		process_corefile(path);
	} while(1);
	closedir(dir);

	if (opted_in >= 2)
		submit_queue();
	else if (opted_in >= 1)
		ask_permission(core_folder);
	return 1;
}
