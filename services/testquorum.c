/*
 * Copyright (c) 2008, 2009 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Christine Caulfield (ccaulfie@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Red Hat, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sched.h>
#include <time.h>

#include <corosync/corotypes.h>
#include <qb/qbipc_common.h>
#include <corosync/corodefs.h>
#include <corosync/logsys.h>
#include <corosync/icmap.h>

#include <corosync/mar_gen.h>
#include <corosync/lcr/lcr_comp.h>
#include <corosync/coroapi.h>

#include <corosync/engine/quorum.h>

LOGSYS_DECLARE_SUBSYS ("TEST");

static void test_init(struct corosync_api_v1 *api, quorum_set_quorate_fn_t report);

/*
 * lcrso object definition
 */
static struct quorum_services_api_ver1 test_quorum_iface_ver0 = {
	.init				= test_init
};

static struct lcr_iface corosync_test_quorum_ver0[1] = {
	{
		.name			= "testquorum",
		.version		= 0,
		.versions_replace	= 0,
		.versions_replace_count	= 0,
		.dependencies		= 0,
		.dependency_count	= 0,
		.constructor		= NULL,
		.destructor		= NULL,
		.interfaces		= (void **)(void *)&test_quorum_iface_ver0,
	},
};

static struct lcr_comp test_quorum_comp_ver0 = {
	.iface_count			= 1,
	.ifaces				= corosync_test_quorum_ver0
};

#ifdef COROSYNC_SOLARIS
void corosync_lcr_component_register (void);

void corosync_lcr_component_register (void) {
	logsys_subsys_init();
#else
__attribute__ ((constructor)) static void corosync_lcr_component_register (void) {
#endif
	lcr_interfaces_set (&corosync_test_quorum_ver0[0], &test_quorum_iface_ver0);
	lcr_component_register (&test_quorum_comp_ver0);
}

/* -------------------------------------------------- */

static quorum_set_quorate_fn_t set_quorum;

static void key_change_notify(
	int32_t event,
	const char *key_name,
	struct icmap_notify_value new_val,
	struct icmap_notify_value old_val,
	void *user_data)
{
	struct memb_ring_id ring_id;
	unsigned int members[1];
	uint8_t u8;

	memset(&ring_id, 0, sizeof(ring_id));
	if (icmap_get_uint8(key_name, &u8) == CS_OK) {
		set_quorum(members, 0, u8, &ring_id);
	}
}

static void quorum_callback(int quorate, void *context)
{
	log_printf(LOGSYS_LEVEL_DEBUG, "quorum callback: quorate = %d\n", quorate);
}

static void test_init(struct corosync_api_v1 *api,
		      quorum_set_quorate_fn_t report)
{

	icmap_track_t icmap_track;

	set_quorum = report;

	/*
	 * Register for icmap changes on quorum.quorate
	 */
	icmap_track_add("quorum.quorate",
		ICMAP_TRACK_ADD | ICMAP_TRACK_DELETE | ICMAP_TRACK_MODIFY,
		key_change_notify,
		NULL,
		&icmap_track);

	/* Register for quorum changes too! */
	api->quorum_register_callback(quorum_callback, NULL);
}
