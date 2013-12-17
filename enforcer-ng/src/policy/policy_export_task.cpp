/*
 * $Id$
 *
 * Copyright (c) 2011 Surfnet 
 * Copyright (c) 2011 .SE (The Internet Infrastructure Foundation).
 * Copyright (c) 2011 OpenDNSSEC AB (svb)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "shared/duration.h"
#include "shared/file.h"
#include "policy/policy_export_task.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "kasp.pb.h"

#include "xmlext-pb/xmlext-rd.h"
#include "xmlext-pb/xmlext-wr.h"

#include "protobuf-orm/pb-orm.h"
#include "daemon/orm.h"

#include <fcntl.h>
#include <memory>

static const char *module_str = "policy_export_task";

void 
perform_policy_export(int sockfd, engineconfig_type *config, const char *policy)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	OrmConnRef conn;
	if (!ods_orm_connect(sockfd, config, conn))
		return; // error already reported.

	{
		OrmTransaction transaction(conn);
		if (!transaction.started()) {
			ods_log_error_and_printf(sockfd, module_str,
					"database transaction failed");
			return;
		}


		{	OrmResultRef rows;
			::ods::kasp::Policy pexport;


			std::string qpolicy;
			if (policy) {
				if (!OrmQuoteStringValue(conn, std::string(policy), qpolicy)) {
					ods_log_error_and_printf(sockfd, module_str,
							"quoting string value failed\n");
					return;
				}
			}

			if (qpolicy.empty()) {
				if (!OrmMessageEnum(conn, pexport.descriptor(), rows)) {
					ods_log_error_and_printf(sockfd, module_str,
							"database policy enumeration failed\n");
					return;
				}
			} else {
				if (!OrmMessageEnumWhere(conn, pexport.descriptor(), rows,
						"name = %s", qpolicy.c_str())) {
					ods_log_error_and_printf(sockfd, module_str,
							"database policy enumeration failed\n");
					return;
				}
			}

			ods_printf(sockfd,
					   "Database set to: %s\n"
					   "Policies:\n"
					   "\n"
					   ,config->datastore);

			for (bool next=OrmFirst(rows); next; next=OrmNext(rows)) {
				ods_printf(sockfd,"1111111\n");
				if (!OrmGetMessage(rows, pexport, true)){
					ods_printf(sockfd,"22222222\n");
					ods_log_error_and_printf(sockfd, module_str,
										"reading policy from database failed");
					return;
				}
				ods_printf(sockfd,"555555555\n");
				if (!write_pb_message_to_xml_fd(pexport.mutable_keys(),sockfd)){
					ods_printf(sockfd,"333333333\n");
					ods_log_error_and_printf(sockfd, module_str,
										"writing message to xml file failed");
					return;
				}
				ods_printf(sockfd,"666666666\n");
			}
			ods_printf(sockfd,"4444444444444\n");
			ods_log_debug("[%s] policy export completed", module_str);
		}
    }
}
