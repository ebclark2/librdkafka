/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2015, Magnus Edenhill
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution. 
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
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdarg.h>

#include "rdkafka_int.h"
#include "rdkafka_request.h"
#include "rdkafka_broker.h"
#include "rdkafka_offset.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_metadata.h"
#include "rdkafka_msgset.h"

#include "rdrand.h"
#include "rdstring.h"

/**
 * Kafka protocol request and response handling.
 * All of this code runs in the broker thread and uses op queues for
 * propagating results back to the various sub-systems operating in
 * other threads.
 */


/* RD_KAFKA_ERR_ACTION_.. to string map */
static const char *rd_kafka_actions_descs[] = {
        "Permanent",
        "Ignore",
        "Refresh",
        "Retry",
        "Inform",
        "Special",
        NULL,
};


/**
 * @brief Decide action(s) to take based on the returned error code.
 *
 * The optional var-args is a .._ACTION_END terminated list
 * of action,error tuples which overrides the general behaviour.
 * It is to be read as: for \p error, return \p action(s).
 *
 * @warning \p request, \p rkbuf and \p rkb may be NULL.
 */
int rd_kafka_err_action (rd_kafka_broker_t *rkb,
			 rd_kafka_resp_err_t err,
			 rd_kafka_buf_t *rkbuf,
			 rd_kafka_buf_t *request, ...) {
	va_list ap;
        int actions = 0;
	int exp_act;
        char actstr[64];

        if (!err)
                return 0;

	/* Match explicitly defined error mappings first. */
	va_start(ap, request);
	while ((exp_act = va_arg(ap, int))) {
		int exp_err = va_arg(ap, int);

		if (err == exp_err)
			actions |= exp_act;
	}
	va_end(ap);

        /* Explicit error match. */
        if (actions) {
                if (err && rkb && request)
                        rd_rkb_dbg(rkb, BROKER, "REQERR",
                                   "%sRequest failed: %s: explicit actions %s",
                                   rd_kafka_ApiKey2str(request->rkbuf_reqhdr.
                                                       ApiKey),
                                   rd_kafka_err2str(err),
                                   rd_flags2str(actstr, sizeof(actstr),
                                                rd_kafka_actions_descs,
                                                actions));

                return actions;
        }

	/* Default error matching */
        switch (err)
        {
        case RD_KAFKA_RESP_ERR_NO_ERROR:
                break;
        case RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION:
        case RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_REPLICA_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_GROUP_COORDINATOR_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR_FOR_GROUP:
        case RD_KAFKA_RESP_ERR__WAIT_COORD:
                /* Request metadata information update */
                actions |= RD_KAFKA_ERR_ACTION_REFRESH;
                break;
        case RD_KAFKA_RESP_ERR__TIMED_OUT:
        case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
                /* Client-side wait-response/in-queue timeout */
        case RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT:
                /* Broker-side request handling timeout */
        case RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS:
        case RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND:
                /* Temporary broker-side problem */
	case RD_KAFKA_RESP_ERR__TRANSPORT:
		/* Broker connection down */
		actions |= RD_KAFKA_ERR_ACTION_RETRY;
		break;
        case RD_KAFKA_RESP_ERR__DESTROY:
	case RD_KAFKA_RESP_ERR_INVALID_SESSION_TIMEOUT:
        case RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE:
        default:
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                break;
        }

        /* If no request buffer was specified, which might be the case
         * in certain error call chains, mask out the retry action. */
        if (!request)
                actions &= ~RD_KAFKA_ERR_ACTION_RETRY;


        if (err && actions && rkb && request)
                rd_rkb_dbg(rkb, BROKER, "REQERR",
                           "%sRequest failed: %s: actions %s",
                           rd_kafka_ApiKey2str(request->rkbuf_reqhdr.ApiKey),
                           rd_kafka_err2str(err),
                           rd_flags2str(actstr, sizeof(actstr),
                                        rd_kafka_actions_descs, actions));

        return actions;
}


/**
 * Send GroupCoordinatorRequest
 */
void rd_kafka_GroupCoordinatorRequest (rd_kafka_broker_t *rkb,
                                       const rd_kafkap_str_t *cgrp,
                                       rd_kafka_replyq_t replyq,
                                       rd_kafka_resp_cb_t *resp_cb,
                                       void *opaque) {
        rd_kafka_buf_t *rkbuf;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_GroupCoordinator, 1,
                                         RD_KAFKAP_STR_SIZE(cgrp));
        rd_kafka_buf_write_kstr(rkbuf, cgrp);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}




/**
 * @brief Parses and handles Offset replies.
 *
 * Returns the parsed offsets (and errors) in \p offsets
 *
 * @returns 0 on success, else an error.
 */
rd_kafka_resp_err_t rd_kafka_handle_Offset (rd_kafka_t *rk,
                                            rd_kafka_broker_t *rkb,
                                            rd_kafka_resp_err_t err,
                                            rd_kafka_buf_t *rkbuf,
                                            rd_kafka_buf_t *request,
                                            rd_kafka_topic_partition_list_t
                                            *offsets) {

        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode = 0;
        int32_t TopicArrayCnt;
        int actions;
        int16_t api_version;

        if (err) {
                ErrorCode = err;
                goto err;
        }

        api_version = request->rkbuf_reqhdr.ApiVersion;

        /* NOTE:
         * Broker may return offsets in a different constellation than
         * in the original request .*/

        rd_kafka_buf_read_i32(rkbuf, &TopicArrayCnt);
        while (TopicArrayCnt-- > 0) {
                rd_kafkap_str_t ktopic;
                int32_t PartArrayCnt;
                char *topic_name;

                rd_kafka_buf_read_str(rkbuf, &ktopic);
                rd_kafka_buf_read_i32(rkbuf, &PartArrayCnt);

                RD_KAFKAP_STR_DUPA(&topic_name, &ktopic);

                while (PartArrayCnt-- > 0) {
                        int32_t kpartition;
                        int32_t OffsetArrayCnt;
                        int64_t Offset = -1;
                        rd_kafka_topic_partition_t *rktpar;

                        rd_kafka_buf_read_i32(rkbuf, &kpartition);
                        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

                        if (api_version == 1) {
                                int64_t Timestamp;
                                rd_kafka_buf_read_i64(rkbuf, &Timestamp);
                                rd_kafka_buf_read_i64(rkbuf, &Offset);
                        } else if (api_version == 0) {
                                rd_kafka_buf_read_i32(rkbuf, &OffsetArrayCnt);
                                /* We only request one offset so just grab
                                 * the first one. */
                                while (OffsetArrayCnt-- > 0)
                                        rd_kafka_buf_read_i64(rkbuf, &Offset);
                        } else {
                                rd_kafka_assert(NULL, !*"NOTREACHED");
                        }

                        rktpar = rd_kafka_topic_partition_list_add(
                                offsets, topic_name, kpartition);
                        rktpar->err = ErrorCode;
                        rktpar->offset = Offset;
                }
        }

        goto done;

 err_parse:
        ErrorCode = rkbuf->rkbuf_err;
 err:
        actions = rd_kafka_err_action(
                rkb, ErrorCode, rkbuf, request,
                RD_KAFKA_ERR_ACTION_PERMANENT,
                RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART,

                RD_KAFKA_ERR_ACTION_REFRESH|RD_KAFKA_ERR_ACTION_RETRY,
                RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION,

                RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                char tmp[256];
                /* Re-query for leader */
                rd_snprintf(tmp, sizeof(tmp),
                            "OffsetRequest failed: %s",
                            rd_kafka_err2str(ErrorCode));
                rd_kafka_metadata_refresh_known_topics(rk, NULL, 1/*force*/,
                                                       tmp);
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                if (rd_kafka_buf_retry(rkb, request))
                        return RD_KAFKA_RESP_ERR__IN_PROGRESS;
                /* FALLTHRU */
        }

done:
        return ErrorCode;
}






/**
 * Send OffsetRequest for toppar 'rktp'.
 */
void rd_kafka_OffsetRequest (rd_kafka_broker_t *rkb,
                             rd_kafka_topic_partition_list_t *partitions,
                             int16_t api_version,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int i;
        size_t of_TopicArrayCnt = 0, of_PartArrayCnt = 0;
        const char *last_topic = "";
        int32_t topic_cnt = 0, part_cnt = 0;

        rd_kafka_topic_partition_list_sort_by_topic(partitions);

        rkbuf = rd_kafka_buf_new_request(
                rkb, RD_KAFKAP_Offset, 1,
                /* ReplicaId+TopicArrayCnt+Topic */
                4+4+100+
                /* PartArrayCnt */
                4 +
                /* partition_cnt * Partition+Time+MaxNumOffs */
                (partitions->cnt * (4+8+4)));

        /* ReplicaId */
        rd_kafka_buf_write_i32(rkbuf, -1);
        /* TopicArrayCnt */
        of_TopicArrayCnt = rd_kafka_buf_write_i32(rkbuf, 0); /* updated later */

        for (i = 0 ; i < partitions->cnt ; i++) {
                const rd_kafka_topic_partition_t *rktpar = &partitions->elems[i];

                if (strcmp(rktpar->topic, last_topic)) {
                        /* Finish last topic, if any. */
                        if (of_PartArrayCnt > 0)
                                rd_kafka_buf_update_i32(rkbuf,
                                                        of_PartArrayCnt,
                                                        part_cnt);

                        /* Topic */
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        topic_cnt++;
                        last_topic = rktpar->topic;
                        /* New topic so reset partition count */
                        part_cnt = 0;

                        /* PartitionArrayCnt: updated later */
                        of_PartArrayCnt = rd_kafka_buf_write_i32(rkbuf, 0);
                }

                /* Partition */
                rd_kafka_buf_write_i32(rkbuf, rktpar->partition);
                part_cnt++;

                /* Time/Offset */
                rd_kafka_buf_write_i64(rkbuf, rktpar->offset);

                if (api_version == 0) {
                        /* MaxNumberOfOffsets */
                        rd_kafka_buf_write_i32(rkbuf, 1);
                }
        }

        if (of_PartArrayCnt > 0) {
                rd_kafka_buf_update_i32(rkbuf, of_PartArrayCnt, part_cnt);
                rd_kafka_buf_update_i32(rkbuf, of_TopicArrayCnt, topic_cnt);
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, api_version,
                                    api_version == 1 ?
                                    RD_KAFKA_FEATURE_OFFSET_TIME : 0);

        rd_rkb_dbg(rkb, TOPIC, "OFFSET",
                   "OffsetRequest (v%hd, opv %d) "
                   "for %"PRId32" topic(s) and %"PRId32" partition(s)",
                   api_version, rkbuf->rkbuf_replyq.version,
                   topic_cnt, partitions->cnt);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}


/**
 * Generic handler for OffsetFetch responses.
 * Offsets for included partitions will be propagated through the passed
 * 'offsets' list.
 *
 * \p update_toppar: update toppar's committed_offset
 */
rd_kafka_resp_err_t
rd_kafka_handle_OffsetFetch (rd_kafka_t *rk,
			     rd_kafka_broker_t *rkb,
			     rd_kafka_resp_err_t err,
			     rd_kafka_buf_t *rkbuf,
			     rd_kafka_buf_t *request,
			     rd_kafka_topic_partition_list_t *offsets,
			     int update_toppar) {
        const int log_decode_errors = LOG_ERR;
        int32_t TopicArrayCnt;
        int64_t offset = RD_KAFKA_OFFSET_INVALID;
        rd_kafkap_str_t metadata;
        int i;
        int actions;
        int seen_cnt = 0;

        if (err)
                goto err;

        /* Set default offset for all partitions. */
        rd_kafka_topic_partition_list_set_offsets(rkb->rkb_rk, offsets, 0,
                                                  RD_KAFKA_OFFSET_INVALID,
						  0 /* !is commit */);

        rd_kafka_buf_read_i32(rkbuf, &TopicArrayCnt);
        for (i = 0 ; i < TopicArrayCnt ; i++) {
                rd_kafkap_str_t topic;
                int32_t PartArrayCnt;
                char *topic_name;
                int j;

                rd_kafka_buf_read_str(rkbuf, &topic);
                rd_kafka_buf_read_i32(rkbuf, &PartArrayCnt);

                RD_KAFKAP_STR_DUPA(&topic_name, &topic);

                for (j = 0 ; j < PartArrayCnt ; j++) {
                        int32_t partition;
                        shptr_rd_kafka_toppar_t *s_rktp;
                        rd_kafka_topic_partition_t *rktpar;
                        int16_t err2;

                        rd_kafka_buf_read_i32(rkbuf, &partition);
                        rd_kafka_buf_read_i64(rkbuf, &offset);
                        rd_kafka_buf_read_str(rkbuf, &metadata);
                        rd_kafka_buf_read_i16(rkbuf, &err2);

                        rktpar = rd_kafka_topic_partition_list_find(offsets,
                                                                    topic_name,
                                                                    partition);
                        if (!rktpar) {
				rd_rkb_dbg(rkb, TOPIC, "OFFSETFETCH",
					   "OffsetFetchResponse: %s [%"PRId32"] "
					   "not found in local list: ignoring",
					   topic_name, partition);
                                continue;
			}

                        seen_cnt++;

			if (!(s_rktp = rktpar->_private)) {
				s_rktp = rd_kafka_toppar_get2(rkb->rkb_rk,
							      topic_name,
							      partition, 0, 0);
				/* May be NULL if topic is not locally known */
				rktpar->_private = s_rktp;
			}

			/* broker reports invalid offset as -1 */
			if (offset == -1)
				rktpar->offset = RD_KAFKA_OFFSET_INVALID;
			else
				rktpar->offset = offset;
                        rktpar->err = err2;

			rd_rkb_dbg(rkb, TOPIC, "OFFSETFETCH",
				   "OffsetFetchResponse: %s [%"PRId32"] offset %"PRId64,
				   topic_name, partition, offset);

			if (update_toppar && !err2 && s_rktp) {
				rd_kafka_toppar_t *rktp = rd_kafka_toppar_s2i(s_rktp);
				/* Update toppar's committed offset */
				rd_kafka_toppar_lock(rktp);
				rktp->rktp_committed_offset = rktpar->offset;
				rd_kafka_toppar_unlock(rktp);
			}


                        if (rktpar->metadata)
                                rd_free(rktpar->metadata);

                        if (RD_KAFKAP_STR_IS_NULL(&metadata)) {
                                rktpar->metadata = NULL;
                                rktpar->metadata_size = 0;
                        } else {
                                rktpar->metadata = RD_KAFKAP_STR_DUP(&metadata);
                                rktpar->metadata_size =
                                        RD_KAFKAP_STR_LEN(&metadata);
                        }
                }
        }


err:
        rd_rkb_dbg(rkb, TOPIC, "OFFFETCH",
                   "OffsetFetch for %d/%d partition(s) returned %s",
                   seen_cnt,
                   offsets ? offsets->cnt : -1, rd_kafka_err2str(err));

        actions = rd_kafka_err_action(rkb, err, rkbuf, request,
				      RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rd_kafka_cgrp_op(rkb->rkb_rk->rk_cgrp, NULL,
                                 RD_KAFKA_NO_REPLYQ,
				 RD_KAFKA_OP_COORD_QUERY, err);
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                if (rd_kafka_buf_retry(rkb, request))
                        return RD_KAFKA_RESP_ERR__IN_PROGRESS;
                /* FALLTHRU */
        }

	return err;

 err_parse:
        err = rkbuf->rkbuf_err;
        goto err;
}



/**
 * @brief Handle OffsetFetch response based on an RD_KAFKA_OP_OFFSET_FETCH
 *        rko in \p opaque.
 *
 * @param opaque rko wrapper for handle_OffsetFetch.
 *
 * The \c rko->rko_u.offset_fetch.partitions list will be filled in with
 * the fetched offsets.
 *
 * A reply will be sent on 'rko->rko_replyq' with type RD_KAFKA_OP_OFFSET_FETCH.
 *
 * @remark \p rkb, \p rkbuf and \p request are optional.
 *
 * @remark The \p request buffer may be retried on error.
 *
 * @locality cgrp's broker thread
 */
void rd_kafka_op_handle_OffsetFetch (rd_kafka_t *rk,
				     rd_kafka_broker_t *rkb,
                                     rd_kafka_resp_err_t err,
                                     rd_kafka_buf_t *rkbuf,
                                     rd_kafka_buf_t *request,
                                     void *opaque) {
        rd_kafka_op_t *rko = opaque;
        rd_kafka_op_t *rko_reply;
        rd_kafka_topic_partition_list_t *offsets;

	RD_KAFKA_OP_TYPE_ASSERT(rko, RD_KAFKA_OP_OFFSET_FETCH);

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* Termination, quick cleanup. */
                rd_kafka_op_destroy(rko);
                return;
        }

        offsets = rd_kafka_topic_partition_list_copy(
                rko->rko_u.offset_fetch.partitions);

        /* If all partitions already had usable offsets then there
         * was no request sent and thus no reply, the offsets list is
         * good to go.. */
        if (rkbuf) {
                /* ..else parse the response (or perror) */
                err = rd_kafka_handle_OffsetFetch(rkb->rkb_rk, rkb, err, rkbuf,
                                                  request, offsets, 0);
                if (err == RD_KAFKA_RESP_ERR__IN_PROGRESS) {
                        rd_kafka_topic_partition_list_destroy(offsets);
                        return; /* Retrying */
                }
        }

        rko_reply = rd_kafka_op_new(RD_KAFKA_OP_OFFSET_FETCH|RD_KAFKA_OP_REPLY);
        rko_reply->rko_err = err;
        rko_reply->rko_u.offset_fetch.partitions = offsets;
        rko_reply->rko_u.offset_fetch.do_free = 1;
	if (rko->rko_rktp)
		rko_reply->rko_rktp = rd_kafka_toppar_keep(
			rd_kafka_toppar_s2i(rko->rko_rktp));

	rd_kafka_replyq_enq(&rko->rko_replyq, rko_reply, 0);

        rd_kafka_op_destroy(rko);
}






/**
 * Send OffsetFetchRequest for toppar.
 *
 * Any partition with a usable offset will be ignored, if all partitions
 * have usable offsets then no request is sent at all but an empty
 * reply is enqueued on the replyq.
 */
void rd_kafka_OffsetFetchRequest (rd_kafka_broker_t *rkb,
                                  int16_t api_version,
                                  rd_kafka_topic_partition_list_t *parts,
				  rd_kafka_replyq_t replyq,
                                  rd_kafka_resp_cb_t *resp_cb,
                                  void *opaque) {
	rd_kafka_buf_t *rkbuf;
        size_t of_TopicCnt;
        int TopicCnt = 0;
        ssize_t of_PartCnt = -1;
        const char *last_topic = NULL;
        int PartCnt = 0;
	int tot_PartCnt = 0;
        int i;

        rkbuf = rd_kafka_buf_new_request(
                rkb, RD_KAFKAP_OffsetFetch, 1,
                RD_KAFKAP_STR_SIZE(rkb->rkb_rk->rk_group_id) +
                4 +
                (parts->cnt * 32));


        /* ConsumerGroup */
        rd_kafka_buf_write_kstr(rkbuf, rkb->rkb_rk->rk_group_id);

        /* Sort partitions by topic */
        rd_kafka_topic_partition_list_sort_by_topic(parts);

	/* TopicArrayCnt */
        of_TopicCnt = rd_kafka_buf_write_i32(rkbuf, 0); /* Updated later */

        for (i = 0 ; i < parts->cnt ; i++) {
                rd_kafka_topic_partition_t *rktpar = &parts->elems[i];

		/* Ignore partitions with a usable offset. */
		if (rktpar->offset != RD_KAFKA_OFFSET_INVALID &&
		    rktpar->offset != RD_KAFKA_OFFSET_STORED) {
			rd_rkb_dbg(rkb, TOPIC, "OFFSET",
				   "OffsetFetchRequest: skipping %s [%"PRId32"] "
				   "with valid offset %s",
				   rktpar->topic, rktpar->partition,
				   rd_kafka_offset2str(rktpar->offset));
			continue;
		}

                if (last_topic == NULL || strcmp(last_topic, rktpar->topic)) {
                        /* New topic */

                        /* Finalize previous PartitionCnt */
                        if (PartCnt > 0)
                                rd_kafka_buf_update_u32(rkbuf, of_PartCnt,
                                                        PartCnt);

                        /* TopicName */
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        /* PartitionCnt, finalized later */
                        of_PartCnt = rd_kafka_buf_write_i32(rkbuf, 0);
                        PartCnt = 0;
			last_topic = rktpar->topic;
                        TopicCnt++;
                }

                /* Partition */
                rd_kafka_buf_write_i32(rkbuf,  rktpar->partition);
                PartCnt++;
		tot_PartCnt++;
        }

        /* Finalize previous PartitionCnt */
        if (PartCnt > 0)
                rd_kafka_buf_update_u32(rkbuf, of_PartCnt,  PartCnt);

        /* Finalize TopicCnt */
        rd_kafka_buf_update_u32(rkbuf, of_TopicCnt, TopicCnt);

        rd_kafka_buf_ApiVersion_set(rkbuf, api_version, 0);

	rd_rkb_dbg(rkb, TOPIC, "OFFSET",
		   "OffsetFetchRequest(v%d) for %d/%d partition(s)",
                   api_version, tot_PartCnt, parts->cnt);

	if (tot_PartCnt == 0) {
		/* No partitions needs OffsetFetch, enqueue empty
		 * response right away. */
                rkbuf->rkbuf_replyq = replyq;
                rkbuf->rkbuf_cb     = resp_cb;
                rkbuf->rkbuf_opaque = opaque;
		rd_kafka_buf_callback(rkb->rkb_rk, rkb, 0, NULL, rkbuf);
		return;
	}

        rd_rkb_dbg(rkb, CGRP|RD_KAFKA_DBG_CONSUMER, "OFFSET",
                   "Fetch committed offsets for %d/%d partition(s)",
                   tot_PartCnt, parts->cnt);


	rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}


/**
 * @remark \p offsets may be NULL if \p err is set
 */
rd_kafka_resp_err_t
rd_kafka_handle_OffsetCommit (rd_kafka_t *rk,
			      rd_kafka_broker_t *rkb,
			      rd_kafka_resp_err_t err,
			      rd_kafka_buf_t *rkbuf,
			      rd_kafka_buf_t *request,
			      rd_kafka_topic_partition_list_t *offsets) {
        const int log_decode_errors = LOG_ERR;
        int32_t TopicArrayCnt;
        int16_t ErrorCode = 0, last_ErrorCode = 0;
	int errcnt = 0;
        int i;
	int actions;

        if (err)
		goto err;

        rd_kafka_buf_read_i32(rkbuf, &TopicArrayCnt);
        for (i = 0 ; i < TopicArrayCnt ; i++) {
                rd_kafkap_str_t topic;
                char *topic_str;
                int32_t PartArrayCnt;
                int j;

                rd_kafka_buf_read_str(rkbuf, &topic);
                rd_kafka_buf_read_i32(rkbuf, &PartArrayCnt);

                RD_KAFKAP_STR_DUPA(&topic_str, &topic);

                for (j = 0 ; j < PartArrayCnt ; j++) {
                        int32_t partition;
                        rd_kafka_topic_partition_t *rktpar;

                        rd_kafka_buf_read_i32(rkbuf, &partition);
                        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

                        rktpar = rd_kafka_topic_partition_list_find(
                                offsets, topic_str, partition);

                        if (!rktpar) {
                                /* Received offset for topic/partition we didn't
                                 * ask for, this shouldn't really happen. */
                                continue;
                        }

                        rktpar->err = ErrorCode;
			if (ErrorCode) {
				last_ErrorCode = ErrorCode;
				errcnt++;
			}
                }
        }

	/* If all partitions failed use error code
	 * from last partition as the global error. */
	if (offsets && errcnt == offsets->cnt)
		err = last_ErrorCode;
	goto done;

 err_parse:
        err = rkbuf->rkbuf_err;

 err:
        actions = rd_kafka_err_action(
		rkb, err, rkbuf, request,

		RD_KAFKA_ERR_ACTION_PERMANENT,
		RD_KAFKA_RESP_ERR_OFFSET_METADATA_TOO_LARGE,

		RD_KAFKA_ERR_ACTION_RETRY,
		RD_KAFKA_RESP_ERR_GROUP_LOAD_IN_PROGRESS,

		RD_KAFKA_ERR_ACTION_REFRESH|RD_KAFKA_ERR_ACTION_SPECIAL,
		RD_KAFKA_RESP_ERR_GROUP_COORDINATOR_NOT_AVAILABLE,

		RD_KAFKA_ERR_ACTION_REFRESH|RD_KAFKA_ERR_ACTION_SPECIAL,
		RD_KAFKA_RESP_ERR_NOT_COORDINATOR_FOR_GROUP,

		RD_KAFKA_ERR_ACTION_REFRESH|RD_KAFKA_ERR_ACTION_RETRY,
		RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION,

		RD_KAFKA_ERR_ACTION_REFRESH|RD_KAFKA_ERR_ACTION_RETRY,
		RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID,

		RD_KAFKA_ERR_ACTION_RETRY,
		RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS,

		RD_KAFKA_ERR_ACTION_PERMANENT,
		RD_KAFKA_RESP_ERR_INVALID_COMMIT_OFFSET_SIZE,

		RD_KAFKA_ERR_ACTION_PERMANENT,
		RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED,

		RD_KAFKA_ERR_ACTION_PERMANENT,
		RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED,

		RD_KAFKA_ERR_ACTION_END);

	if (actions & RD_KAFKA_ERR_ACTION_REFRESH && rk->rk_cgrp) {
		/* Mark coordinator dead or re-query for coordinator.
		 * ..dead() will trigger a re-query. */
		if (actions & RD_KAFKA_ERR_ACTION_SPECIAL)
			rd_kafka_cgrp_coord_dead(rk->rk_cgrp, err,
						 "OffsetCommitRequest failed");
		else
			rd_kafka_cgrp_coord_query(rk->rk_cgrp,
						  "OffsetCommitRequest failed");
	}
	if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
		if (rd_kafka_buf_retry(rkb, request))
			return RD_KAFKA_RESP_ERR__IN_PROGRESS;
		/* FALLTHRU */
	}

 done:
	return err;
}




/**
 * @brief Send OffsetCommitRequest for a list of partitions.
 *
 * @returns 0 if none of the partitions in \p offsets had valid offsets,
 *          else 1.
 */
int rd_kafka_OffsetCommitRequest (rd_kafka_broker_t *rkb,
                                   rd_kafka_cgrp_t *rkcg,
                                   int16_t api_version,
                                   rd_kafka_topic_partition_list_t *offsets,
                                   rd_kafka_replyq_t replyq,
                                   rd_kafka_resp_cb_t *resp_cb,
                                   void *opaque, const char *reason) {
	rd_kafka_buf_t *rkbuf;
        ssize_t of_TopicCnt = -1;
        int TopicCnt = 0;
        const char *last_topic = NULL;
        ssize_t of_PartCnt = -1;
        int PartCnt = 0;
	int tot_PartCnt = 0;
        int i;

        rd_kafka_assert(NULL, offsets != NULL);

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_OffsetCommit,
                                         1, 100 + (offsets->cnt * 128));

        /* ConsumerGroup */
        rd_kafka_buf_write_kstr(rkbuf, rkcg->rkcg_group_id);

        /* v1,v2 */
        if (api_version >= 1) {
                /* ConsumerGroupGenerationId */
                rd_kafka_buf_write_i32(rkbuf, rkcg->rkcg_generation_id);
                /* ConsumerId */
                rd_kafka_buf_write_kstr(rkbuf, rkcg->rkcg_member_id);
                /* v2: RetentionTime */
                if (api_version == 2)
                        rd_kafka_buf_write_i64(rkbuf, -1);
        }

        /* Sort offsets by topic */
        rd_kafka_topic_partition_list_sort_by_topic(offsets);

        /* TopicArrayCnt: Will be updated when we know the number of topics. */
        of_TopicCnt = rd_kafka_buf_write_i32(rkbuf, 0);

        for (i = 0 ; i < offsets->cnt ; i++) {
                rd_kafka_topic_partition_t *rktpar = &offsets->elems[i];

		/* Skip partitions with invalid offset. */
		if (rktpar->offset < 0)
			continue;

                if (last_topic == NULL || strcmp(last_topic, rktpar->topic)) {
                        /* New topic */

                        /* Finalize previous PartitionCnt */
                        if (PartCnt > 0)
                                rd_kafka_buf_update_u32(rkbuf, of_PartCnt,
                                                        PartCnt);

                        /* TopicName */
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        /* PartitionCnt, finalized later */
                        of_PartCnt = rd_kafka_buf_write_i32(rkbuf, 0);
                        PartCnt = 0;
			last_topic = rktpar->topic;
                        TopicCnt++;
                }

                /* Partition */
                rd_kafka_buf_write_i32(rkbuf,  rktpar->partition);
                PartCnt++;
		tot_PartCnt++;

                /* Offset */
                rd_kafka_buf_write_i64(rkbuf, rktpar->offset);

                /* v1: TimeStamp */
                if (api_version == 1)
                        rd_kafka_buf_write_i64(rkbuf, -1);// FIXME: retention time

                /* Metadata */
		/* Java client 0.9.0 and broker <0.10.0 can't parse
		 * Null metadata fields, so as a workaround we send an
		 * empty string if it's Null. */
		if (!rktpar->metadata)
			rd_kafka_buf_write_str(rkbuf, "", 0);
		else
			rd_kafka_buf_write_str(rkbuf,
					       rktpar->metadata,
					       rktpar->metadata_size);
        }

	if (tot_PartCnt == 0) {
		/* No topic+partitions had valid offsets to commit. */
		rd_kafka_replyq_destroy(&replyq);
		rd_kafka_buf_destroy(rkbuf);
		return 0;
	}

        /* Finalize previous PartitionCnt */
        if (PartCnt > 0)
                rd_kafka_buf_update_u32(rkbuf, of_PartCnt,  PartCnt);

        /* Finalize TopicCnt */
        rd_kafka_buf_update_u32(rkbuf, of_TopicCnt, TopicCnt);

        rd_kafka_buf_ApiVersion_set(rkbuf, api_version, 0);

        rd_rkb_dbg(rkb, TOPIC, "OFFSET",
                   "Enqueue OffsetCommitRequest(v%d, %d/%d partition(s))): %s",
                   api_version, tot_PartCnt, offsets->cnt, reason);

	rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

	return 1;

}



/**
 * @brief Write "consumer" protocol type MemberState for SyncGroupRequest to
 *        enveloping buffer \p rkbuf.
 */
static void rd_kafka_group_MemberState_consumer_write (
        rd_kafka_buf_t *env_rkbuf,
        const rd_kafka_group_member_t *rkgm) {
        rd_kafka_buf_t *rkbuf;
        int i;
        const char *last_topic = NULL;
        size_t of_TopicCnt;
        ssize_t of_PartCnt = -1;
        int TopicCnt = 0;
        int PartCnt = 0;
        rd_slice_t slice;

        rkbuf = rd_kafka_buf_new(1, 100);
        rd_kafka_buf_write_i16(rkbuf, 0); /* Version */
        of_TopicCnt = rd_kafka_buf_write_i32(rkbuf, 0); /* Updated later */
        for (i = 0 ; i < rkgm->rkgm_assignment->cnt ; i++) {
                const rd_kafka_topic_partition_t *rktpar;

                rktpar = &rkgm->rkgm_assignment->elems[i];

                if (!last_topic || strcmp(last_topic,
                                          rktpar->topic)) {
                        if (last_topic)
                                /* Finalize previous PartitionCnt */
                                rd_kafka_buf_update_i32(rkbuf, of_PartCnt,
                                                        PartCnt);
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        /* Updated later */
                        of_PartCnt = rd_kafka_buf_write_i32(rkbuf, 0);
                        PartCnt = 0;
                        last_topic = rktpar->topic;
                        TopicCnt++;
                }

                rd_kafka_buf_write_i32(rkbuf, rktpar->partition);
                PartCnt++;
        }

        if (of_PartCnt != -1)
                rd_kafka_buf_update_i32(rkbuf, of_PartCnt, PartCnt);
        rd_kafka_buf_update_i32(rkbuf, of_TopicCnt, TopicCnt);

        rd_kafka_buf_write_kbytes(rkbuf, rkgm->rkgm_userdata);

        /* Get pointer to binary buffer */
        rd_slice_init_full(&slice, &rkbuf->rkbuf_buf);

        /* Write binary buffer as Kafka Bytes to enveloping buffer. */
        rd_kafka_buf_write_i32(env_rkbuf, (int32_t)rd_slice_remains(&slice));
        rd_buf_write_slice(&env_rkbuf->rkbuf_buf, &slice);

        rd_kafka_buf_destroy(rkbuf);
}

/**
 * Send SyncGroupRequest
 */
void rd_kafka_SyncGroupRequest (rd_kafka_broker_t *rkb,
                                const rd_kafkap_str_t *group_id,
                                int32_t generation_id,
                                const rd_kafkap_str_t *member_id,
                                const rd_kafka_group_member_t
                                *assignments,
                                int assignment_cnt,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int i;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_SyncGroup,
                                         1,
                                         RD_KAFKAP_STR_SIZE(group_id) +
                                         4 /* GenerationId */ +
                                         RD_KAFKAP_STR_SIZE(member_id) +
                                         4 /* array size group_assignment */ +
                                         (assignment_cnt * 100/*guess*/));
        rd_kafka_buf_write_kstr(rkbuf, group_id);
        rd_kafka_buf_write_i32(rkbuf, generation_id);
        rd_kafka_buf_write_kstr(rkbuf, member_id);
        rd_kafka_buf_write_i32(rkbuf, assignment_cnt);

        for (i = 0 ; i < assignment_cnt ; i++) {
                const rd_kafka_group_member_t *rkgm = &assignments[i];

                rd_kafka_buf_write_kstr(rkbuf, rkgm->rkgm_member_id);
                rd_kafka_group_MemberState_consumer_write(rkbuf, rkgm);
        }

        /* This is a blocking request */
        rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_BLOCKING;
        rd_kafka_buf_set_abs_timeout(
                rkbuf,
                rkb->rkb_rk->rk_conf.group_session_timeout_ms +
                3000/* 3s grace period*/,
                0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}

/**
 * Handler for SyncGroup responses
 * opaque must be the cgrp handle.
 */
void rd_kafka_handle_SyncGroup (rd_kafka_t *rk,
				rd_kafka_broker_t *rkb,
                                rd_kafka_resp_err_t err,
                                rd_kafka_buf_t *rkbuf,
                                rd_kafka_buf_t *request,
                                void *opaque) {
        rd_kafka_cgrp_t *rkcg = opaque;
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode = 0;
        rd_kafkap_bytes_t MemberState = RD_ZERO_INIT;
        int actions;

	if (rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC) {
		rd_kafka_dbg(rkb->rkb_rk, CGRP, "SYNCGROUP",
			     "SyncGroup response: discarding outdated request "
			     "(now in join-state %s)",
			     rd_kafka_cgrp_join_state_names[rkcg->
							    rkcg_join_state]);
		return;
	}

        if (err) {
                ErrorCode = err;
                goto err;
        }

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
        rd_kafka_buf_read_bytes(rkbuf, &MemberState);

err:
        actions = rd_kafka_err_action(rkb, ErrorCode, rkbuf, request,
				      RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rd_kafka_cgrp_op(rkcg, NULL, RD_KAFKA_NO_REPLYQ,
				 RD_KAFKA_OP_COORD_QUERY,
                                 ErrorCode);
                /* FALLTHRU */
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                if (rd_kafka_buf_retry(rkb, request))
                        return;
                /* FALLTHRU */
        }

        rd_kafka_dbg(rkb->rkb_rk, CGRP, "SYNCGROUP",
                     "SyncGroup response: %s (%d bytes of MemberState data)",
                     rd_kafka_err2str(ErrorCode),
                     RD_KAFKAP_BYTES_LEN(&MemberState));

        if (ErrorCode == RD_KAFKA_RESP_ERR__DESTROY)
                return; /* Termination */

        rd_kafka_cgrp_handle_SyncGroup(rkcg, rkb, ErrorCode, &MemberState);

        return;

 err_parse:
        ErrorCode = rkbuf->rkbuf_err;
        goto err;
}


/**
 * Send JoinGroupRequest
 */
void rd_kafka_JoinGroupRequest (rd_kafka_broker_t *rkb,
                                const rd_kafkap_str_t *group_id,
                                const rd_kafkap_str_t *member_id,
                                const rd_kafkap_str_t *protocol_type,
				const rd_list_t *topics,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque) {
        rd_kafka_buf_t *rkbuf;
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_assignor_t *rkas;
        int i;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_JoinGroup,
                                         1,
                                         RD_KAFKAP_STR_SIZE(group_id) +
                                         4 /* sessionTimeoutMs */ +
                                         RD_KAFKAP_STR_SIZE(member_id) +
                                         RD_KAFKAP_STR_SIZE(protocol_type) +
                                         4 /* array count GroupProtocols */ +
                                         (rd_list_cnt(topics) * 100));
        rd_kafka_buf_write_kstr(rkbuf, group_id);
        rd_kafka_buf_write_i32(rkbuf, rk->rk_conf.group_session_timeout_ms);
        rd_kafka_buf_write_kstr(rkbuf, member_id);
        rd_kafka_buf_write_kstr(rkbuf, protocol_type);
        rd_kafka_buf_write_i32(rkbuf, rk->rk_conf.enabled_assignor_cnt);

        RD_LIST_FOREACH(rkas, &rk->rk_conf.partition_assignors, i) {
                rd_kafkap_bytes_t *member_metadata;
		if (!rkas->rkas_enabled)
			continue;
                rd_kafka_buf_write_kstr(rkbuf, rkas->rkas_protocol_name);
                member_metadata = rkas->rkas_get_metadata_cb(rkas, topics);
                rd_kafka_buf_write_kbytes(rkbuf, member_metadata);
                rd_kafkap_bytes_destroy(member_metadata);
        }

        /* This is a blocking request */
        rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_BLOCKING;
        rd_kafka_buf_set_abs_timeout(
                rkbuf,
                rk->rk_conf.group_session_timeout_ms +
                3000/* 3s grace period*/,
                0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}






/**
 * Send LeaveGroupRequest
 */
void rd_kafka_LeaveGroupRequest (rd_kafka_broker_t *rkb,
                                 const rd_kafkap_str_t *group_id,
                                 const rd_kafkap_str_t *member_id,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_LeaveGroup,
                                         1,
                                         RD_KAFKAP_STR_SIZE(group_id) +
                                         RD_KAFKAP_STR_SIZE(member_id));
        rd_kafka_buf_write_kstr(rkbuf, group_id);
        rd_kafka_buf_write_kstr(rkbuf, member_id);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}


/**
 * Handler for LeaveGroup responses
 * opaque must be the cgrp handle.
 */
void rd_kafka_handle_LeaveGroup (rd_kafka_t *rk,
				 rd_kafka_broker_t *rkb,
                                 rd_kafka_resp_err_t err,
                                 rd_kafka_buf_t *rkbuf,
                                 rd_kafka_buf_t *request,
                                 void *opaque) {
        rd_kafka_cgrp_t *rkcg = opaque;
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode = 0;
        int actions;

        if (err) {
                ErrorCode = err;
                goto err;
        }

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

err:
        actions = rd_kafka_err_action(rkb, ErrorCode, rkbuf, request,
				      RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rd_kafka_cgrp_op(rkcg, NULL, RD_KAFKA_NO_REPLYQ,
				 RD_KAFKA_OP_COORD_QUERY, ErrorCode);
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                if (rd_kafka_buf_retry(rkb, request))
                        return;
                /* FALLTHRU */
        }

        if (ErrorCode)
                rd_kafka_dbg(rkb->rkb_rk, CGRP, "LEAVEGROUP",
                             "LeaveGroup response: %s",
                             rd_kafka_err2str(ErrorCode));

        return;

 err_parse:
        ErrorCode = rkbuf->rkbuf_err;
        goto err;
}






/**
 * Send HeartbeatRequest
 */
void rd_kafka_HeartbeatRequest (rd_kafka_broker_t *rkb,
                                const rd_kafkap_str_t *group_id,
                                int32_t generation_id,
                                const rd_kafkap_str_t *member_id,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque) {
        rd_kafka_buf_t *rkbuf;

        rd_rkb_dbg(rkb, CGRP, "HEARTBEAT",
                   "Heartbeat for group \"%s\" generation id %"PRId32,
                   group_id->str, generation_id);

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_Heartbeat,
                                         1,
                                         RD_KAFKAP_STR_SIZE(group_id) +
                                         4 /* GenerationId */ +
                                         RD_KAFKAP_STR_SIZE(member_id));

        rd_kafka_buf_write_kstr(rkbuf, group_id);
        rd_kafka_buf_write_i32(rkbuf, generation_id);
        rd_kafka_buf_write_kstr(rkbuf, member_id);

        rd_kafka_buf_set_abs_timeout(
                rkbuf,
                rkb->rkb_rk->rk_conf.group_session_timeout_ms,
                0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}




/**
 * Send ListGroupsRequest
 */
void rd_kafka_ListGroupsRequest (rd_kafka_broker_t *rkb,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_ListGroups, 0, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}


/**
 * Send DescribeGroupsRequest
 */
void rd_kafka_DescribeGroupsRequest (rd_kafka_broker_t *rkb,
                                     const char **groups, int group_cnt,
                                     rd_kafka_replyq_t replyq,
                                     rd_kafka_resp_cb_t *resp_cb,
                                     void *opaque) {
        rd_kafka_buf_t *rkbuf;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_DescribeGroups,
                                         1, 32*group_cnt);

        rd_kafka_buf_write_i32(rkbuf, group_cnt);
        while (group_cnt-- > 0)
                rd_kafka_buf_write_str(rkbuf, groups[group_cnt], -1);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}




/**
 * @brief Generic handler for Metadata responses
 *
 * @locality rdkafka main thread
 */
static void rd_kafka_handle_Metadata (rd_kafka_t *rk,
                                      rd_kafka_broker_t *rkb,
                                      rd_kafka_resp_err_t err,
                                      rd_kafka_buf_t *rkbuf,
                                      rd_kafka_buf_t *request,
                                      void *opaque) {
        rd_kafka_op_t *rko = opaque; /* Possibly NULL */
        struct rd_kafka_metadata *md = NULL;
        const rd_list_t *topics = request->rkbuf_u.Metadata.topics;
        char actstr[64];
        int actions;

        rd_kafka_assert(NULL, err == RD_KAFKA_RESP_ERR__DESTROY ||
                        thrd_is_current(rk->rk_thread));

        /* Avoid metadata updates when we're terminating. */
        if (rd_kafka_terminating(rkb->rkb_rk) ||
            err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* Terminating */
                goto done;
        }

        if (err)
                goto err;

        if (!topics)
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "===== Received metadata: %s =====",
                           request->rkbuf_u.Metadata.reason);
        else
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "===== Received metadata "
                           "(for %d requested topics): %s =====",
                           rd_list_cnt(topics),
                           request->rkbuf_u.Metadata.reason);

        err = rd_kafka_parse_Metadata(rkb, request, rkbuf, &md);
        if (err)
                goto err;

        if (rko && rko->rko_replyq.q) {
                /* Reply to metadata requester, passing on the metadata.
                 * Reuse requesting rko for the reply. */
                rko->rko_err = err;
                rko->rko_u.metadata.md = md;

                rd_kafka_replyq_enq(&rko->rko_replyq, rko, 0);
                rko = NULL;
        } else {
                if (md)
                        rd_free(md);
        }

        goto done;

 err:
        actions = rd_kafka_err_action(
                rkb, err, rkbuf, request,

                RD_KAFKA_ERR_ACTION_RETRY,
                RD_KAFKA_RESP_ERR__PARTIAL,

                RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                if (rd_kafka_buf_retry(rkb, request))
                        return;
                /* FALLTHRU */
        } else {
                rd_rkb_log(rkb, LOG_WARNING, "METADATA",
                           "Metadata request failed: %s: %s (%dms): %s",
                           request->rkbuf_u.Metadata.reason,
                           rd_kafka_err2str(err),
                           (int)(request->rkbuf_ts_sent/1000),
                           rd_flags2str(actstr, sizeof(actstr),
                                        rd_kafka_actions_descs,
                                        actions));
        }



        /* FALLTHRU */

 done:
        if (rko)
                rd_kafka_op_destroy(rko);
}


/**
 * @brief Construct MetadataRequest (does not send)
 *
 * \p topics is a list of topic names (char *) to request.
 *
 * !topics          - only request brokers (if supported by broker, else
 *                    all topics)
 *  topics.cnt==0   - all topics in cluster are requested
 *  topics.cnt >0   - only specified topics are requested
 *
 * @param reason    - metadata request reason
 * @param rko       - (optional) rko with replyq for handling response.
 *                    Specifying an rko forces a metadata request even if
 *                    there is already a matching one in-transit.
 *
 * If full metadata for all topics is requested (or all brokers, which
 * results in all-topics on older brokers) and there is already a full request
 * in transit then this function will return RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS
 * otherwise RD_KAFKA_RESP_ERR_NO_ERROR. If \p rko is non-NULL the request
 * is sent regardless.
 */
rd_kafka_resp_err_t
rd_kafka_MetadataRequest (rd_kafka_broker_t *rkb,
                          const rd_list_t *topics, const char *reason,
                          rd_kafka_op_t *rko) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        int topic_cnt = topics ? rd_list_cnt(topics) : 0;
        int *full_incr = NULL;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(rkb,
                                                          RD_KAFKAP_Metadata,
                                                          0, 2,
                                                          &features);

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_Metadata, 1,
                                         4 + (50 * topic_cnt));

        if (!reason)
                reason = "";

        rkbuf->rkbuf_u.Metadata.reason = rd_strdup(reason);

        if (!topics && ApiVersion >= 1) {
                /* a null(0) array (in the protocol) represents no topics */
                rd_kafka_buf_write_i32(rkbuf, 0);
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "Request metadata for brokers only: %s", reason);
                full_incr = &rkb->rkb_rk->rk_metadata_cache.
                        rkmc_full_brokers_sent;

        } else {
                if (topic_cnt == 0 && !rko)
                        full_incr = &rkb->rkb_rk->rk_metadata_cache.
                                rkmc_full_topics_sent;

                if (topic_cnt == 0 && ApiVersion >= 1)
                        rd_kafka_buf_write_i32(rkbuf, -1); /* Null: all topics*/
                else
                        rd_kafka_buf_write_i32(rkbuf, topic_cnt);

                if (topic_cnt == 0) {
                        rkbuf->rkbuf_u.Metadata.all_topics = 1;
                        rd_rkb_dbg(rkb, METADATA, "METADATA",
                                   "Request metadata for all topics: "
                                   "%s", reason);
                } else
                        rd_rkb_dbg(rkb, METADATA, "METADATA",
                                   "Request metadata for %d topic(s): "
                                   "%s", topic_cnt, reason);
        }

        if (full_incr) {
                /* Avoid multiple outstanding full requests
                 * (since they are redundant and side-effect-less).
                 * Forced requests (app using metadata() API) are passed
                 * through regardless. */

                mtx_lock(&rkb->rkb_rk->rk_metadata_cache.
                         rkmc_full_lock);
                if (*full_incr > 0 && (!rko || !rko->rko_u.metadata.force)) {
                        mtx_unlock(&rkb->rkb_rk->rk_metadata_cache.
                                   rkmc_full_lock);
                        rd_rkb_dbg(rkb, METADATA, "METADATA",
                                   "Skipping metadata request: %s: "
                                   "full request already in-transit",
                                   reason);
                        rd_kafka_buf_destroy(rkbuf);
                        return RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS;
                }

                (*full_incr)++;
                mtx_unlock(&rkb->rkb_rk->rk_metadata_cache.
                           rkmc_full_lock);
                rkbuf->rkbuf_u.Metadata.decr = full_incr;
                rkbuf->rkbuf_u.Metadata.decr_lock = &rkb->rkb_rk->
                        rk_metadata_cache.rkmc_full_lock;
        }


        if (topic_cnt > 0) {
                char *topic;
                int i;

                /* Maintain a copy of the topics list so we can purge
                 * hints from the metadata cache on error. */
                rkbuf->rkbuf_u.Metadata.topics =
                        rd_list_copy(topics, rd_list_string_copy, NULL);

                RD_LIST_FOREACH(topic, topics, i)
                        rd_kafka_buf_write_str(rkbuf, topic, -1);

        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        /* Metadata requests are part of the important control plane
         * and should go before other requests (Produce, Fetch, etc). */
        rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_FLASH;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf,
                                       /* Handle response thru rk_ops,
                                        * but forward parsed result to
                                        * rko's replyq when done. */
                                       RD_KAFKA_REPLYQ(rkb->rkb_rk->
                                                       rk_ops, 0),
                                       rd_kafka_handle_Metadata, rko);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}









/**
 * @brief Parses and handles ApiVersion reply.
 *
 * @param apis will be allocated, populated and sorted
 *             with broker's supported APIs.
 * @param api_cnt will be set to the number of elements in \p *apis

 * @returns 0 on success, else an error.
 */
rd_kafka_resp_err_t
rd_kafka_handle_ApiVersion (rd_kafka_t *rk,
			    rd_kafka_broker_t *rkb,
			    rd_kafka_resp_err_t err,
			    rd_kafka_buf_t *rkbuf,
			    rd_kafka_buf_t *request,
			    struct rd_kafka_ApiVersion **apis,
			    size_t *api_cnt) {
        const int log_decode_errors = LOG_ERR;
	int32_t ApiArrayCnt;
	int16_t ErrorCode;
	int i = 0;

	*apis = NULL;

        if (err)
                goto err;

	rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
	if ((err = ErrorCode))
		goto err;

        rd_kafka_buf_read_i32(rkbuf, &ApiArrayCnt);
	if (ApiArrayCnt > 1000)
		rd_kafka_buf_parse_fail(rkbuf,
					"ApiArrayCnt %"PRId32" out of range",
					ApiArrayCnt);

	rd_rkb_dbg(rkb, FEATURE, "APIVERSION",
		   "Broker API support:");

	*apis = malloc(sizeof(**apis) * ApiArrayCnt);

	for (i = 0 ; i < ApiArrayCnt ; i++) {
		struct rd_kafka_ApiVersion *api = &(*apis)[i];

		rd_kafka_buf_read_i16(rkbuf, &api->ApiKey);
		rd_kafka_buf_read_i16(rkbuf, &api->MinVer);
		rd_kafka_buf_read_i16(rkbuf, &api->MaxVer);

		rd_rkb_dbg(rkb, FEATURE, "APIVERSION",
			   "  ApiKey %s (%hd) Versions %hd..%hd",
			   rd_kafka_ApiKey2str(api->ApiKey),
			   api->ApiKey, api->MinVer, api->MaxVer);
        }

	*api_cnt = ApiArrayCnt;
        qsort(*apis, *api_cnt, sizeof(**apis), rd_kafka_ApiVersion_key_cmp);

	goto done;

 err_parse:
        err = rkbuf->rkbuf_err;
 err:
	if (*apis)
		rd_free(*apis);

        /* There are no retryable errors. */

 done:
        return err;
}



/**
 * Send ApiVersionRequest (KIP-35)
 */
void rd_kafka_ApiVersionRequest (rd_kafka_broker_t *rkb,
				 rd_kafka_replyq_t replyq,
				 rd_kafka_resp_cb_t *resp_cb,
				 void *opaque, int flash_msg) {
        rd_kafka_buf_t *rkbuf;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_ApiVersion, 1, 4);
	rkbuf->rkbuf_flags |= (flash_msg ? RD_KAFKA_OP_F_FLASH : 0);
	rd_kafka_buf_write_i32(rkbuf, 0); /* Empty array: request all APIs */

	/* Non-supporting brokers will tear down the connection when they
	 * receive an unknown API request, so dont retry request on failure. */
	rkbuf->rkbuf_retries = RD_KAFKA_BUF_NO_RETRIES;

	/* 0.9.0.x brokers will not close the connection on unsupported
	 * API requests, so we minimize the timeout for the request.
	 * This is a regression on the broker part. */
        rd_kafka_buf_set_abs_timeout(
                rkbuf,
                rkb->rkb_rk->rk_conf.api_version_request_timeout_ms,
                0);

        if (replyq.q)
                rd_kafka_broker_buf_enq_replyq(rkb,
                                               rkbuf, replyq, resp_cb, opaque);
	else /* in broker thread */
		rd_kafka_broker_buf_enq1(rkb, rkbuf, resp_cb, opaque);
}


/**
 * Send SaslHandshakeRequest (KIP-43)
 */
void rd_kafka_SaslHandshakeRequest (rd_kafka_broker_t *rkb,
				    const char *mechanism,
				    rd_kafka_replyq_t replyq,
				    rd_kafka_resp_cb_t *resp_cb,
				    void *opaque, int flash_msg) {
        rd_kafka_buf_t *rkbuf;
	int mechlen = (int)strlen(mechanism);

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_SaslHandshake,
                                         1, RD_KAFKAP_STR_SIZE0(mechlen));
	rkbuf->rkbuf_flags |= (flash_msg ? RD_KAFKA_OP_F_FLASH : 0);
	rd_kafka_buf_write_str(rkbuf, mechanism, mechlen);

	/* Non-supporting brokers will tear down the conneciton when they
	 * receive an unknown API request or where the SASL GSSAPI
	 * token type is not recognized, so dont retry request on failure. */
	rkbuf->rkbuf_retries = RD_KAFKA_BUF_NO_RETRIES;

	/* 0.9.0.x brokers will not close the connection on unsupported
	 * API requests, so we minimize the timeout of the request.
	 * This is a regression on the broker part. */
        if (!rkb->rkb_rk->rk_conf.api_version_request &&
            rkb->rkb_rk->rk_conf.socket_timeout_ms > 10*1000)
                rd_kafka_buf_set_abs_timeout(rkbuf, 10*1000 /*10s*/, 0);

	if (replyq.q)
		rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq,
                                               resp_cb, opaque);
	else /* in broker thread */
		rd_kafka_broker_buf_enq1(rkb, rkbuf, resp_cb, opaque);
}




/**
 * @brief Parses a Produce reply.
 * @returns 0 on success or an error code on failure.
 * @locality broker thread
 */
static rd_kafka_resp_err_t
rd_kafka_handle_Produce_parse (rd_kafka_broker_t *rkb,
                               rd_kafka_toppar_t *rktp,
                               rd_kafka_buf_t *rkbuf,
                               rd_kafka_buf_t *request,
                               int64_t *offsetp,
                               int64_t *timestampp) {
        int32_t TopicArrayCnt;
        int32_t PartitionArrayCnt;
        struct {
                int32_t Partition;
                int16_t ErrorCode;
                int64_t Offset;
        } hdr;
        const int log_decode_errors = LOG_ERR;

        rd_kafka_buf_read_i32(rkbuf, &TopicArrayCnt);
        if (TopicArrayCnt != 1)
                goto err;

        /* Since we only produce to one single topic+partition in each
         * request we assume that the reply only contains one topic+partition
         * and that it is the same that we requested.
         * If not the broker is buggy. */
        rd_kafka_buf_skip_str(rkbuf);
        rd_kafka_buf_read_i32(rkbuf, &PartitionArrayCnt);

        if (PartitionArrayCnt != 1)
                goto err;

        rd_kafka_buf_read_i32(rkbuf, &hdr.Partition);
        rd_kafka_buf_read_i16(rkbuf, &hdr.ErrorCode);
        rd_kafka_buf_read_i64(rkbuf, &hdr.Offset);

        *offsetp = hdr.Offset;

        *timestampp = -1;
        if (request->rkbuf_reqhdr.ApiVersion >= 2) {
                rd_kafka_buf_read_i64(rkbuf, timestampp);
        }

        if (request->rkbuf_reqhdr.ApiVersion >= 1) {
                int32_t Throttle_Time;
                rd_kafka_buf_read_i32(rkbuf, &Throttle_Time);

                rd_kafka_op_throttle_time(rkb, rkb->rkb_rk->rk_rep,
                                          Throttle_Time);
        }


        return hdr.ErrorCode;

 err_parse:
        return rkbuf->rkbuf_err;
 err:
        return RD_KAFKA_RESP_ERR__BAD_MSG;
}


/**
 * @brief Handle ProduceResponse
 *
 * @locality broker thread
 */
static void rd_kafka_handle_Produce (rd_kafka_t *rk,
                                     rd_kafka_broker_t *rkb,
                                     rd_kafka_resp_err_t err,
                                     rd_kafka_buf_t *reply,
                                     rd_kafka_buf_t *request,
                                     void *opaque) {
        shptr_rd_kafka_toppar_t *s_rktp = opaque; /* from ProduceRequest() */
        rd_kafka_toppar_t *rktp = rd_kafka_toppar_s2i(s_rktp);
        int64_t offset = RD_KAFKA_OFFSET_INVALID;
        int64_t timestamp = -1;

        /* Parse Produce reply (unless the request errored) */
        if (!err && reply)
                err = rd_kafka_handle_Produce_parse(rkb, rktp,
                                                    reply, request,
                                                    &offset, &timestamp);


        if (likely(!err)) {
                rd_rkb_dbg(rkb, MSG, "MSGSET",
                           "%s [%"PRId32"]: MessageSet with %i message(s) "
                           "delivered",
                           rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                           request->rkbuf_msgq.rkmq_msg_cnt);

        } else {
                /* Error */
                int actions;
                char actstr[64];

                if (err == RD_KAFKA_RESP_ERR__DESTROY)
                        goto done; /* Terminating */

                actions = rd_kafka_err_action(
                        rkb, err, reply, request,

                        RD_KAFKA_ERR_ACTION_REFRESH,
                        RD_KAFKA_RESP_ERR__TRANSPORT,

                        RD_KAFKA_ERR_ACTION_REFRESH,
                        RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART,

                        RD_KAFKA_ERR_ACTION_RETRY,
                        RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS,

                        RD_KAFKA_ERR_ACTION_RETRY,
                        RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND,

                        RD_KAFKA_ERR_ACTION_RETRY,
                        RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE,

                        RD_KAFKA_ERR_ACTION_RETRY,
                        RD_KAFKA_RESP_ERR__TIMED_OUT,

                        RD_KAFKA_ERR_ACTION_PERMANENT,
                        RD_KAFKA_RESP_ERR__MSG_TIMED_OUT,

                        RD_KAFKA_ERR_ACTION_END);

                rd_rkb_dbg(rkb, MSG, "MSGSET",
                           "%s [%"PRId32"]: MessageSet with %i message(s) "
                           "encountered error: %s (actions %s)",
                           rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                           request->rkbuf_msgq.rkmq_msg_cnt,
                           rd_kafka_err2str(err),
                           rd_flags2str(actstr, sizeof(actstr),
                                        rd_kafka_actions_descs,
                                        actions));


                if (actions & (RD_KAFKA_ERR_ACTION_REFRESH |
                               RD_KAFKA_ERR_ACTION_RETRY)) {
                        /* Retry */
                        int incr_retry = 1; /* Increase per-message retry cnt */

                        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                                /* Request metadata information update.
                                 * These errors imply that we have stale
                                 * information and the request was
                                 * either rejected or not sent -
                                 * we don't need to increment the retry count
                                 * when we perform a retry since:
                                 *   - it is a temporary error (hopefully)
                                 *   - there is no chance of duplicate delivery
                                 */
                                rd_kafka_toppar_leader_unavailable(
                                        rktp, "produce", err);

                                /* We can't be certain the request wasn't
                                 * sent in case of transport failure,
                                 * so the ERR__TRANSPORT case will need
                                 * the retry count to be increased */
                                if (err != RD_KAFKA_RESP_ERR__TRANSPORT)
                                        incr_retry = 0;
                        }

                        /* If message timed out in queue, not in transit,
                         * we will retry at a later time but not increment
                         * the retry count since there is no risk
                         * of duplicates. */
                        if (!rd_kafka_buf_was_sent(request))
                                incr_retry = 0;

                        /* Since requests are specific to a broker
                         * we move the retryable messages from the request
                         * back to the partition queue (prepend) and then
                         * let the new broker construct a new request.
                         * While doing this we also make sure the retry count
                         * for each message is honoured, any messages that
                         * would exceeded the retry count will not be
                         * moved but instead fail below. */
                        rd_kafka_toppar_retry_msgq(rktp, &request->rkbuf_msgq,
                                                   incr_retry);

                        if (rd_kafka_msgq_len(&request->rkbuf_msgq) == 0) {
                                /* No need do anything more with the request
                                 * here since the request no longer has any
                                 messages associated with it. */
                                goto done;
                        }
                }

                /* Translate request-level timeout error code
                 * to message-level timeout error code. */
                if (err == RD_KAFKA_RESP_ERR__TIMED_OUT ||
                    err == RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE)
                        err = RD_KAFKA_RESP_ERR__MSG_TIMED_OUT;

                /* Fatal errors: no message transmission retries */
                /* FALLTHRU */
        }

        /* Propagate assigned offset and timestamp back to app. */
        if (likely(!err && offset != RD_KAFKA_OFFSET_INVALID)) {
                rd_kafka_msg_t *rkm;
                if (rktp->rktp_rkt->rkt_conf.produce_offset_report) {
                        /* produce.offset.report: each message */
                        TAILQ_FOREACH(rkm, &request->rkbuf_msgq.rkmq_msgs,
                                      rkm_link) {
                                rkm->rkm_offset = offset++;
                                if (timestamp != -1) {
                                        rkm->rkm_timestamp = timestamp;
                                        rkm->rkm_tstype = RD_KAFKA_MSG_ATTR_LOG_APPEND_TIME;
                                }
                        }
                } else {
                        /* Last message in each batch */
                        rkm = TAILQ_LAST(&request->rkbuf_msgq.rkmq_msgs,
                                         rd_kafka_msg_head_s);
                        rkm->rkm_offset = offset +
                                request->rkbuf_msgq.rkmq_msg_cnt - 1;
                        if (timestamp != -1) {
                                rkm->rkm_timestamp = timestamp;
                                rkm->rkm_tstype = RD_KAFKA_MSG_ATTR_LOG_APPEND_TIME;
                        }
                }
        }

        /* Enqueue messages for delivery report */
        rd_kafka_dr_msgq(rktp->rktp_rkt, &request->rkbuf_msgq, err);

 done:
        rd_kafka_toppar_destroy(s_rktp); /* from ProduceRequest() */
}


/**
 * @brief Send ProduceRequest for messages in toppar queue.
 *
 * @returns the number of messages included, or 0 on error / no messages.
 *
 * @locality broker thread
 */
int rd_kafka_ProduceRequest (rd_kafka_broker_t *rkb, rd_kafka_toppar_t *rktp) {
        rd_kafka_buf_t *rkbuf;
        rd_kafka_itopic_t *rkt = rktp->rktp_rkt;
        size_t MessageSetSize = 0;
        int cnt;
        rd_ts_t now;
        int64_t first_msg_timeout;
        int tmout;

        /**
         * Create ProduceRequest with as many messages from the toppar
         * transmit queue as possible.
         */
        rkbuf = rd_kafka_msgset_create_ProduceRequest(rkb, rktp,
                                                      &MessageSetSize);
        if (unlikely(!rkbuf))
                return 0;

        cnt = rkbuf->rkbuf_msgq.rkmq_msg_cnt;
        rd_dassert(cnt > 0);

        rd_avg_add(&rktp->rktp_rkt->rkt_avg_batchcnt, (int64_t)cnt);
        rd_avg_add(&rktp->rktp_rkt->rkt_avg_batchsize, (int64_t)MessageSetSize);

        if (!rkt->rkt_conf.required_acks)
                rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_NO_RESPONSE;

        /* Use timeout from first message in batch */
        now = rd_clock();
        first_msg_timeout = (TAILQ_FIRST(&rkbuf->rkbuf_msgq.rkmq_msgs)->
                             rkm_ts_timeout - now) / 1000;

        if (unlikely(first_msg_timeout <= 0)) {
                /* Message has already timed out, allow 100 ms
                 * to produce anyway */
                tmout = 100;
        } else {
                tmout = (int)RD_MIN(INT_MAX, first_msg_timeout);
        }

        /* Set absolute timeout (including retries), the
         * effective timeout for this specific request will be
         * capped by socket.timeout.ms */
        rd_kafka_buf_set_abs_timeout(rkbuf, tmout, now);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf,
                                       RD_KAFKA_NO_REPLYQ,
                                       rd_kafka_handle_Produce,
                                       /* toppar ref for handle_Produce() */
                                       rd_kafka_toppar_keep(rktp));

        return cnt;
}


/**
 * @brief Construct and send CreateTopicsRequest to \p rkb
 *        with the topics (NewTopic_t*) in \p new_topics, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_CreateTopicsRequest (rd_kafka_broker_t *rkb,
                              const rd_list_t *new_topics /*(NewTopic_t*)*/,
                              rd_kafka_AdminOptions_t *options,
                              char *errstr, size_t errstr_size,
                              rd_kafka_replyq_t replyq,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        int i = 0;
        rd_kafka_NewTopic_t *newt;
        int op_timeout;

        if (rd_list_cnt(new_topics) == 0) {
                rd_snprintf(errstr, errstr_size, "No topics to create");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
                rkb, RD_KAFKAP_CreateTopics, 0, 2, &features);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "Topic Admin API (KIP-4) not supported "
                            "by broker, requires broker version >= 0.10.2.0");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        if (rd_kafka_confval_get_int(&options->validate_only) &&
            ApiVersion < 1) {
                rd_snprintf(errstr, errstr_size,
                            "CreateTopics.validate_only=true not "
                            "supported by broker");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }



        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_CreateTopics, 1,
                                         4 +
                                         (rd_list_cnt(new_topics) * 200) +
                                         4 + 1);

        /* #topics */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(new_topics));

        while ((newt = rd_list_elem(new_topics, i++))) {
                int partition;
                int ei = 0;
                const rd_kafka_ConfigEntry_t *entry;

                /* topic */
                rd_kafka_buf_write_str(rkbuf, newt->topic, -1);

                if (rd_list_cnt(&newt->replicas)) {
                        /* num_partitions and replication_factor must be
                         * set to -1 if a replica assignment is sent. */
                        /* num_partitions */
                        rd_kafka_buf_write_i32(rkbuf, -1);
                        /* replication_factor */
                        rd_kafka_buf_write_i16(rkbuf, -1);
                } else {
                        /* num_partitions */
                        rd_kafka_buf_write_i32(rkbuf, newt->num_partitions);
                        /* replication_factor */
                        rd_kafka_buf_write_i16(rkbuf,
                                               (int16_t)newt->
                                               replication_factor);
                }

                /* #replica_assignment */
                rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(&newt->replicas));

                /* Replicas per partition, see rdkafka_admin.[ch]
                 * for how these are constructed. */
                for (partition = 0 ; partition < rd_list_cnt(&newt->replicas);
                     partition++) {
                        const rd_list_t *replicas;
                        int ri = 0;

                        replicas = rd_list_elem(&newt->replicas, partition);
                        if (!replicas)
                                continue;

                        /* partition */
                        rd_kafka_buf_write_i32(rkbuf, partition);
                        /* #replicas */
                        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(replicas));

                        for (ri = 0 ; ri < rd_list_cnt(replicas) ; ri++) {
                                /* replica */
                                rd_kafka_buf_write_i32(
                                        rkbuf, rd_list_get_int32(replicas, ri));
                        }
                }

                /* #config_entries */
                rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(&newt->config));

                RD_LIST_FOREACH(entry, &newt->config, ei) {
                        /* config_name */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->name, -1);
                        /* config_value (nullable) */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->value, -1);
                }
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        rd_kafka_buf_write_i32(rkbuf, op_timeout);

        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout+1000, 0);

        if (ApiVersion >= 1) {
                /* validate_only */
                rd_kafka_buf_write_i8(rkbuf,
                                      rd_kafka_confval_get_int(&options->
                                                               validate_only));
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send DeleteTopicsRequest to \p rkb
 *        with the topics (DeleteTopic_t *) in \p del_topics, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_DeleteTopicsRequest (rd_kafka_broker_t *rkb,
                              const rd_list_t *del_topics /*(DeleteTopic_t*)*/,
                              rd_kafka_AdminOptions_t *options,
                              char *errstr, size_t errstr_size,
                              rd_kafka_replyq_t replyq,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        int i = 0;
        rd_kafka_DeleteTopic_t *delt;
        int op_timeout;

        if (rd_list_cnt(del_topics) == 0) {
                rd_snprintf(errstr, errstr_size, "No topics to delete");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
                rkb, RD_KAFKAP_DeleteTopics, 0, 1, &features);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "Topic Admin API (KIP-4) not supported "
                            "by broker, requires broker version >= 0.10.2.0");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_DeleteTopics, 1,
                                         /* FIXME */
                                         4 +
                                         (rd_list_cnt(del_topics) * 100) +
                                         4);

        /* #topics */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(del_topics));

        while ((delt = rd_list_elem(del_topics, i++)))
                rd_kafka_buf_write_str(rkbuf, delt->topic, -1);

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        rd_kafka_buf_write_i32(rkbuf, op_timeout);

        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout+1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @brief Construct and send CreatePartitionsRequest to \p rkb
 *        with the topics (NewPartitions_t*) in \p new_parts, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_CreatePartitionsRequest (rd_kafka_broker_t *rkb,
                                  const rd_list_t *new_parts /*(NewPartitions_t*)*/,
                                  rd_kafka_AdminOptions_t *options,
                                  char *errstr, size_t errstr_size,
                                  rd_kafka_replyq_t replyq,
                                  rd_kafka_resp_cb_t *resp_cb,
                                  void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int i = 0;
        rd_kafka_NewPartitions_t *newp;
        int op_timeout;

        if (rd_list_cnt(new_parts) == 0) {
                rd_snprintf(errstr, errstr_size, "No partitions to create");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
                rkb, RD_KAFKAP_CreatePartitions, 0, 0, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "CreatePartitions (KIP-195) not supported "
                            "by broker, requires broker version >= 1.0.0");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_CreatePartitions, 1,
                                         4 +
                                         (rd_list_cnt(new_parts) * 200) +
                                         4 + 1);

        /* #topics */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(new_parts));

        while ((newp = rd_list_elem(new_parts, i++))) {
                /* topic */
                rd_kafka_buf_write_str(rkbuf, newp->topic, -1);

                /* New partition count */
                rd_kafka_buf_write_i32(rkbuf, (int32_t)newp->total_cnt);

                /* #replica_assignment */
                if (rd_list_empty(&newp->replicas)) {
                        rd_kafka_buf_write_i32(rkbuf, -1);
                } else {
                        const rd_list_t *replicas;
                        int pi = -1;

                        rd_kafka_buf_write_i32(rkbuf,
                                               rd_list_cnt(&newp->replicas));

                        while ((replicas = rd_list_elem(&newp->replicas,
                                                        ++pi))) {
                                int ri = 0;

                                /* replica count */
                                rd_kafka_buf_write_i32(rkbuf,
                                                       rd_list_cnt(replicas));

                                /* replica */
                                for (ri = 0 ; ri < rd_list_cnt(replicas) ;
                                     ri++) {
                                        rd_kafka_buf_write_i32(
                                                rkbuf,
                                                rd_list_get_int32(replicas,
                                                                  ri));
                                }
                        }
                }
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        rd_kafka_buf_write_i32(rkbuf, op_timeout);

        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout+1000, 0);

        /* validate_only */
        rd_kafka_buf_write_i8(
                rkbuf, rd_kafka_confval_get_int(&options->validate_only));

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send AlterConfigsRequest to \p rkb
 *        with the configs (ConfigResource_t*) in \p configs, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_AlterConfigsRequest (rd_kafka_broker_t *rkb,
                              const rd_list_t *configs /*(ConfigResource_t*)*/,
                              rd_kafka_AdminOptions_t *options,
                              char *errstr, size_t errstr_size,
                              rd_kafka_replyq_t replyq,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int i;
        const rd_kafka_ConfigResource_t *config;
        int op_timeout;

        if (rd_list_cnt(configs) == 0) {
                rd_snprintf(errstr, errstr_size,
                            "No config resources specified");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
                rkb, RD_KAFKAP_AlterConfigs, 0, 0, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "AlterConfigs (KIP-133) not supported "
                            "by broker, requires broker version >= 0.11.0");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        /* incremental requires ApiVersion > FIXME */
        if (ApiVersion < 1 /* FIXME */ &&
            rd_kafka_confval_get_int(&options->incremental)) {
                rd_snprintf(errstr, errstr_size,
                            "AlterConfigs.incremental=true (KIP-248) "
                            "not supported by broker, "
                            "requires broker version >= 2.0.0");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_AlterConfigs, 1,
                                         rd_list_cnt(configs) * 200);

        /* #resources */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(configs));

        RD_LIST_FOREACH(config, configs, i) {
                const rd_kafka_ConfigEntry_t *entry;
                int ei;

                /* resource_type */
                rd_kafka_buf_write_i8(rkbuf, config->restype);

                /* resource_name */
                rd_kafka_buf_write_str(rkbuf, config->name, -1);

                /* #config */
                rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(&config->config));

                RD_LIST_FOREACH(entry, &config->config, ei) {
                        /* config_name */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->name, -1);
                        /* config_value (nullable) */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->value, -1);

                        if (ApiVersion == 1)
                                rd_kafka_buf_write_i8(rkbuf,
                                                      entry->a.operation);
                        else if (entry->a.operation != RD_KAFKA_ALTER_OP_SET) {
                                rd_snprintf(errstr, errstr_size,
                                            "Broker version >= 2.0.0 required "
                                            "for add/delete config "
                                            "entries: only set supported "
                                            "by this broker");
                                rd_kafka_buf_destroy(rkbuf);
                                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                        }
                }
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout+1000, 0);

        /* validate_only */
        rd_kafka_buf_write_i8(
                rkbuf, rd_kafka_confval_get_int(&options->validate_only));

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send DescribeConfigsRequest to \p rkb
 *        with the configs (ConfigResource_t*) in \p configs, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_DescribeConfigsRequest (rd_kafka_broker_t *rkb,
                                 const rd_list_t *configs /*(ConfigResource_t*)*/,
                                 rd_kafka_AdminOptions_t *options,
                                 char *errstr, size_t errstr_size,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int i;
        const rd_kafka_ConfigResource_t *config;
        int op_timeout;

        if (rd_list_cnt(configs) == 0) {
                rd_snprintf(errstr, errstr_size,
                            "No config resources specified");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
                rkb, RD_KAFKAP_DescribeConfigs, 0, 1, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "DescribeConfigs (KIP-133) not supported "
                            "by broker, requires broker version >= 0.11.0");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_DescribeConfigs, 1,
                                         rd_list_cnt(configs) * 200);

        /* #resources */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(configs));

        RD_LIST_FOREACH(config, configs, i) {
                const rd_kafka_ConfigEntry_t *entry;
                int ei;

                /* resource_type */
                rd_kafka_buf_write_i8(rkbuf, config->restype);

                /* resource_name */
                rd_kafka_buf_write_str(rkbuf, config->name, -1);

                /* #config */
                if (rd_list_empty(&config->config)) {
                        /* Get all configs */
                        rd_kafka_buf_write_i32(rkbuf, -1);
                } else {
                        /* Get requested configs only */
                        rd_kafka_buf_write_i32(rkbuf,
                                               rd_list_cnt(&config->config));
                }

                RD_LIST_FOREACH(entry, &config->config, ei) {
                        /* config_name */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->name, -1);
                }
        }


        if (ApiVersion == 1) {
                /* include_synonyms */
                rd_kafka_buf_write_i8(rkbuf, 1);
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout+1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}
