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

#include <asm/unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <signal.h>

#include "corewatcher.h"

int do_unlink = 0;
int uid;
int sig;
char *corefile;
char *package;
char *component;
char *arch;

#define MAX(A,B) ((A) > (B) ? (A) : (B))

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

void get_package_info(char *appfile) {
	char *command = NULL, *line = NULL;
	char *c;
	FILE *file;
	int ret = 0;
	size_t size = 0;

	if (asprintf(&command, "rpm -q --whatprovides %s --queryformat \"%%{NAME}-%%{VERSION}-%%{RELEASE}-%%{ARCH}\"", appfile) < 0) {
		line = strdup("Unknown");
	} else {
		file = popen(command, "r");

		ret = getline(&line, &size, file);
		if ((!size) || (ret < 0))
			line = strdup("Unknown");
		c = strchr(line, '\n');
		if (c) *c = 0;
		pclose(file);
	}
	package = strdup(line);

	if (asprintf(&command, "rpm -q --whatprovides %s --queryformat \"%%{NAME}\"", appfile) < 0) {
		line = strdup("Unknown");
	} else {
		file = popen(command, "r");
		ret = getline(&line, &size, file);
		if ((!size) || (ret < 0))
			line = strdup("Unknown");
		c = strchr(line, '\n');
		if (c) *c = 0;
		pclose(file);
	}
	component = strdup(line);

	if (asprintf(&command, "rpm -q --whatprovides %s --queryformat \"%%{ARCH}\"", appfile) < 0) {
		line = strdup("Unknown");
	} else {
		file = popen(command, "r");
		ret = getline(&line, &size, file);
		if ((!size) || (ret < 0))
			line = strdup("Unknown");
		c = strchr(line, '\n');
		if (c) *c = 0;
		pclose(file);
	}
	arch = strdup(line);
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
		return line;
	}

	file = popen(command, "r");

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
	char *release = get_release();
	char *kernel = get_kernel();
	struct timeval tv;

	gettimeofday(&tv, NULL);
	get_package_info(appfile);

	ret = asprintf(&result,
		       "analyzer: corewatcher-gdb\n"
		       "architecture: %s\n"
		       "component: %s\n"
		       "coredump: %s\n"
		       "executable: %s\n"
		       "kernel: %s\n"
		       "package: %s\n"
		       "reason: Process %s was killed by signal %d (%s)\n"
		       "release: %s\n"
		       "time: %lu\n"
		       "uid: %d\n"
		       "\nbacktrace\n-----\n",
		       arch,
		       component,
		       corefile,
		       appfile,
		       kernel,
		       package,
		       appfile, sig, signame(sig),
		       release,
		       tv.tv_sec,
		       uid);

	free(kernel);
	free(package);
	free(release);

	if (ret < 0)
		result = strdup("Unknown");

	return result;
}

char *extract_core(char *corefile)
{
	char *command = NULL, *c1 = NULL, *c2 = NULL, *line;
	char *coredump = NULL;
	char *appfile;
	FILE *file;

	coredump = find_coredump(corefile);
	if (!coredump)
		return NULL;
	appfile = find_executable(coredump);
	if (!appfile)
		return NULL;

	c1 = build_core_header(appfile, corefile);

	if (asprintf(&command, "LANG=C gdb --batch -f %s %s -x /var/lib/corewatcher/gdb.command 2> /dev/null", appfile, corefile) < 0)
		return NULL;

	file = popen(command, "r");
	while (!feof(file)) {
		size_t size = 0;
		int ret;

		c2 = c1;
		line = NULL;
		ret = getline(&line, &size, file);
		if (!size)
			break;
		if (ret < 0)
			break;

		if (strstr(line, "no debugging symbols found")) {
			free(line);
			continue;
		}
		if (strstr(line, "reason: ")) {
			free(line);
			continue;
		}

		if (c1) {
			c1 = NULL;
			if (asprintf(&c1, "%s%s", c2, line) < 0)
				continue;
			free(c2);
		} else {
			c1 = NULL;
			if (asprintf(&c1, "%s", line) < 0)
				continue;
		}
		free(line);
	}
	pclose(file);
	free(command);
	return c1;
}

void process_corefile(char *filename)
{
	char *ptr;
	char newfile[8192];
	ptr = extract_core(filename);

	if (!ptr) {
		unlink(filename);
		return;
	}

	queue_backtrace(ptr);
	fprintf(stderr, "---[start of coredump]---\n%s\n---[end of coredump]---\n", ptr);
	sprintf(newfile,"%s.processed", filename);
	if (do_unlink)
		unlink(filename);
	else
		rename(filename, newfile);

	free(ptr);
}


int scan_dmesg(void __unused *unused)
{
	DIR *dir;
	struct dirent *entry;
	char path[PATH_MAX*2];

	dir = opendir("/tmp/");
	if (!dir)
		return 1;

	fprintf(stderr, "+ scanning...\n");
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

	if (opted_in >= 2)
		submit_queue();
	else if (opted_in >= 1)
		ask_permission();
	return 1;
}
