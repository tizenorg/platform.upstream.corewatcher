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


#ifndef __INCLUDE_GUARD_KERNELOOPS_H_
#define __INCLUDE_GUARD_KERNELOOPS_H_

/* borrowed from the kernel */
#define barrier() __asm__ __volatile__("": : :"memory")
#define __unused  __attribute__ ((__unused__))

#define MAX_URLS 9

extern void queue_backtrace(char *oops);
extern void submit_queue(void);
extern void clear_queue(void);

extern int scan_dmesg(void * unused);
extern void read_config_file(char *filename);

extern void ask_permission(void);
extern void dbus_ask_permission(char * detail_file_name);
extern void dbus_say_thanks(char *url);

extern int opted_in;
extern int allow_distro_to_pass_on;
extern char *submit_url[MAX_URLS];
extern int url_count;
extern char *build_release;

extern int testmode;
extern int pinged;

extern char *find_executable(char *);
extern char *find_coredump(char *);
extern int uid;
extern int sig;


#endif
