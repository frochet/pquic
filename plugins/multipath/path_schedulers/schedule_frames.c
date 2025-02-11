#include "../bpf.h"

#define copy_for_duplication(cnx, bpfdd, packet, bytes, length, data_bytes, pure_ack, cong_controlled) { \
   \
   if (data_bytes && (bpfdd->data_length + data_bytes) < MAX_DUPLICATE_DATA_LENGTH) { \
        /* Let's copy data for further duplication in next packet */    \
        my_memcpy(&bpfdd->data[bpfdd->data_length], &bytes[length-data_bytes], data_bytes);   \
        bpfdd->requires_duplication = 1;   \
        bpfdd->data_length += data_bytes;   \
        set_pkt(packet, AK_PKT_IS_PURE_ACK, get_pkt(packet, AK_PKT_IS_PURE_ACK) && pure_ack);   \
        set_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED, get_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED) || cong_controlled);   \
        \
        /* And requires waking now */   \
        set_cnx(cnx, AK_CNX_WAKE_NOW, 0, 1);   \
    } \
}

protoop_arg_t schedule_frames(picoquic_cnx_t *cnx) {
    picoquic_packet_t* packet = (picoquic_packet_t*) get_cnx(cnx, AK_CNX_INPUT, 0);
    size_t send_buffer_max = (size_t) get_cnx(cnx, AK_CNX_INPUT, 1);
    uint64_t current_time = (uint64_t) get_cnx(cnx, AK_CNX_INPUT, 2);
    picoquic_packet_t* retransmit_p = (picoquic_packet_t*) get_cnx(cnx, AK_CNX_INPUT, 3);
    picoquic_path_t* from_path = (picoquic_path_t*) get_cnx(cnx, AK_CNX_INPUT, 4);
    char* reason = (char*) get_cnx(cnx, AK_CNX_INPUT, 5);

    int ret = 0;
    uint32_t length = 0;
    int is_cleartext_mode = 0;
    uint32_t checksum_overhead = helper_get_checksum_length(cnx, is_cleartext_mode);

    bpf_duplicate_data *bpfdd = get_bpf_duplicate_data(cnx);

    /* FIXME cope with different path MTUs */
    picoquic_path_t *sending_path = schedule_path(cnx, retransmit_p, from_path, reason, bpfdd->requires_duplication);
    PUSH_LOG_CTX(cnx, "\"sending path\": \"%p\"", (protoop_arg_t) sending_path);
    uint32_t sending_path_mtu = (uint32_t) get_path(sending_path, AK_PATH_SEND_MTU, 0);

    uint32_t send_buffer_min_max = (send_buffer_max > sending_path_mtu) ? sending_path_mtu : (uint32_t)send_buffer_max;
    int retransmit_possible = 1;
    picoquic_packet_context_enum pc = picoquic_packet_context_application;
    size_t data_bytes = 0;
    uint32_t header_length = 0;
    uint8_t* bytes = (uint8_t *) get_pkt(packet, AK_PKT_BYTES);
    picoquic_packet_type_enum packet_type = picoquic_packet_1rtt_protected_phi0;

    /* TODO: manage multiple streams. */
    picoquic_stream_head* stream = NULL;
    int tls_ready = helper_is_tls_stream_ready(cnx);
    stream = helper_find_ready_stream(cnx);

    picoquic_stream_head* plugin_stream = helper_find_ready_plugin_stream(cnx);

    /* First enqueue frames that can be fairly sent, if any */
    /* Only schedule new frames if there is no planned frames */

    picoquic_packet_type_enum ptype = (picoquic_packet_type_enum) get_pkt(packet, AK_PKT_TYPE);
    char * retrans_reason = NULL;

    if (ret == 0 && retransmit_possible &&
        (length = helper_retransmit_needed(cnx, pc, sending_path, current_time, packet, send_buffer_min_max, &is_cleartext_mode, &header_length, &retrans_reason)) > 0) {
        if (reason != NULL) {
            protoop_arg_t args[1];
            args[0] = (protoop_arg_t) packet;
            run_noparam(cnx, retrans_reason, 1, (protoop_arg_t *) &args, NULL);
        }
        /* Set the new checksum length */
        checksum_overhead = helper_get_checksum_length(cnx, is_cleartext_mode);
        /* Check whether it makes sense to add an ACK at the end of the retransmission */
        /* Don't do that if it risks mixing clear text and encrypted ack */
        picoquic_path_t *path_0 = (picoquic_path_t *) get_cnx(cnx, AK_CNX_PATH, 0);
        if (is_cleartext_mode == 0 && ptype != picoquic_packet_0rtt_protected) {
            if (sending_path == path_0 && helper_prepare_ack_frame(cnx, current_time, pc, &bytes[length], send_buffer_min_max - checksum_overhead - length, &data_bytes) == 0) {
                length += (uint32_t)data_bytes;
                set_pkt(packet, AK_PKT_LENGTH, length);
            }
        }
        /* document the send time & overhead */
        set_pkt(packet, AK_PKT_IS_PURE_ACK, 0);
        set_pkt(packet, AK_PKT_SEND_TIME, current_time);
        set_pkt(packet, AK_PKT_CHECKSUM_OVERHEAD, checksum_overhead);
    }
    else if (ret == 0) {
        uint64_t cwin = get_path(sending_path, AK_PATH_CWIN, 0);
        uint64_t bytes_in_transit = get_path(sending_path, AK_PATH_BYTES_IN_TRANSIT, 0);
        picoquic_packet_context_t *pkt_ctx = (picoquic_packet_context_t *) get_path(sending_path, AK_PATH_PKT_CTX, pc);
        int challenge_verified = (int) get_path(sending_path, AK_PATH_CHALLENGE_VERIFIED, 0);
        uint64_t challenge_time = (uint64_t) get_path(sending_path, AK_PATH_CHALLENGE_TIME, 0);
        uint64_t retransmit_timer = (uint64_t) get_path(sending_path, AK_PATH_RETRANSMIT_TIMER, 0);
        queue_t *retry_frames = (queue_t *) get_cnx(cnx, AK_CNX_RETRY_FRAMES, 0);
        queue_t *rtx_frames = (queue_t *) get_cnx(cnx, AK_CNX_RTX_FRAMES, pc);

        length = helper_predict_packet_header_length(
                cnx, packet_type, sending_path);
        set_pkt(packet, AK_PKT_TYPE, packet_type);
        set_pkt(packet, AK_PKT_OFFSET, length);
        header_length = length;
        uint64_t sequence_number = (uint64_t) get_pkt_ctx(pkt_ctx, AK_PKTCTX_SEND_SEQUENCE);
        set_pkt(packet, AK_PKT_SEQUENCE_NUMBER, sequence_number);
        set_pkt(packet, AK_PKT_SEND_TIME, current_time);
        set_pkt(packet, AK_PKT_SEND_PATH, (protoop_arg_t) sending_path);

        int mtu_needed = helper_is_mtu_probe_needed(cnx, sending_path);
        int handshake_done_to_send = !get_cnx(cnx, AK_CNX_CLIENT_MODE, 0) && get_cnx(cnx, AK_CNX_HANDSHAKE_DONE, 0) && !get_cnx(cnx, AK_CNX_HANDSHAKE_DONE_SENT, 0);
        bpf_data *bpfd = get_bpf_data(cnx);
        uniflow_data_t *sending_uniflow = mp_get_sending_uniflow_data(bpfd, sending_path);

        /* We first need to check if there is ANY receive path that requires acknowledgement, and also no path response to send */
        int any_receiving_require_ack = 0;
        int receive_require_ack = 0;
        int path_challenge_response_to_send = 0;
        for (int i = 0; i < bpfd->nb_receiving_proposed; i++) {
            picoquic_path_t *receive_path = bpfd->receiving_uniflows[i]->path;
            if (receive_path != NULL) {
                receive_require_ack |= (helper_is_ack_needed(cnx, current_time, pc, receive_path) & 1) << i;
                /* Handle here the reception of the ping */
                receive_require_ack |= (get_path(receive_path, AK_PATH_PING_RECEIVED, 0) & 1) << i;
                any_receiving_require_ack |= receive_require_ack & (1 << i);
                path_challenge_response_to_send |= (get_path(receive_path, AK_PATH_CHALLENGE_RESPONSE_TO_SEND, 0) & 1) << i;
            }
        }

        if (any_receiving_require_ack == 0
            && path_challenge_response_to_send == 0
            && (!sending_uniflow || sending_uniflow->has_sent_uniflows_frame == 1)
            && (challenge_verified == 1 || current_time < challenge_time + retransmit_timer)
            && mtu_needed) {
            if (ret == 0 && send_buffer_max > sending_path_mtu) {
                length = helper_prepare_mtu_probe(cnx, sending_path, header_length, checksum_overhead, bytes);
                set_pkt(packet, AK_PKT_IS_MTU_PROBE, 1);
                set_pkt(packet, AK_PKT_LENGTH, length);
                set_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED, 0);  // See DPLPMTUD draft
                set_path(sending_path, AK_PATH_MTU_PROBE_SENT, 0, 1);
                set_pkt(packet, AK_PKT_IS_PURE_ACK, 0);
            } else {
                PROTOOP_PRINTF(cnx, "Trying to send MTU probe on path %p, but blocked by CWIN %" PRIu64 " and BIF %" PRIu64 " MTU needed %d stream %p tls_ready %d send_buffer_max %d path_send_mtu %d ret %d\n", (protoop_arg_t) sending_path, cwin, bytes_in_transit, mtu_needed, (protoop_arg_t) stream, tls_ready, send_buffer_max, sending_path_mtu, ret);
                length = header_length;
            }
        }

        /* If we are blocked by something, let's send control frames */
        if (length == header_length) {
            if (challenge_verified == 0 &&
                current_time >= (challenge_time + retransmit_timer)) {
                if (helper_prepare_path_challenge_frame(cnx, &bytes[length],
                                                        send_buffer_min_max - checksum_overhead - length,
                                                        &data_bytes, sending_path) == 0) {
                    length += (uint32_t) data_bytes;
                    set_path(sending_path, AK_PATH_CHALLENGE_TIME, 0, current_time);
                    uint8_t challenge_repeat_count = (uint8_t) get_path(sending_path, AK_PATH_CHALLENGE_REPEAT_COUNT, 0);
                    challenge_repeat_count++;
                    set_path(sending_path, AK_PATH_CHALLENGE_REPEAT_COUNT, 0, challenge_repeat_count);
                    set_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED, 1);
                    PROTOOP_PRINTF(cnx, "Sending path %p CWIN %" PRIu64 " BIF %" PRIu64 "\n", (protoop_arg_t) sending_path, cwin, bytes_in_transit);
                    if (challenge_repeat_count > MAX_RECEIVING_UNIFLOWS * PICOQUIC_CHALLENGE_REPEAT_MAX) {
                        PROTOOP_PRINTF(cnx, "%s\n", (protoop_arg_t) "Too many challenge retransmits, disconnect");
                        picoquic_set_cnx_state(cnx, picoquic_state_disconnected);
                        helper_callback_function(cnx, 0, NULL, 0, picoquic_callback_close);
                        length = 0;
                    }
                }
            }
            bool path_validation_in_progress = challenge_verified == 0 && (challenge_time == 0 || current_time < challenge_time + retransmit_timer);

            picoquic_state_enum cnx_state = get_cnx(cnx, AK_CNX_STATE, 0);

            if (cnx_state != picoquic_state_disconnected) {
                picoquic_path_t *path_0 = (picoquic_path_t *) get_cnx(cnx, AK_CNX_PATH, 0);

                /* Before going further, let's duplicate all required frames first */
                if (!path_validation_in_progress && bpfdd->requires_duplication) { // TODO: What about no validated path available for duplication ?
                    if (bpfdd->data_length <= send_buffer_min_max - checksum_overhead - length) {
                        my_memcpy(&bytes[length], bpfdd->data, bpfdd->data_length);
                        length += (uint32_t) bpfdd->data_length;
                    }
                    bpfdd->data_length = 0;
                    bpfdd->requires_duplication = 0;
                }

                /* FIXME I know Multipath somewhat bypass the reservation rules, but it is required here and easier like this... */
                if (bpfd->nb_sending_proposed > 0 && bpfd->nb_receiving_proposed > 0 && (get_cnx(cnx, AK_CNX_HANDSHAKE_DONE, 0) && (get_cnx(cnx, AK_CNX_CLIENT_MODE, 0) || get_cnx(cnx, AK_CNX_HANDSHAKE_DONE_ACKED, 0))) &&
                    sending_uniflow && !sending_uniflow->has_sent_uniflows_frame) {
                    reserve_uniflows_frame(cnx, sending_uniflow);
                    sending_uniflow->has_sent_uniflows_frame = 1;
                }

                if (any_receiving_require_ack) {
                    for (int i = 0; i < bpfd->nb_receiving_proposed; i++) {
                        uniflow_data_t *udtmp = bpfd->receiving_uniflows[i];
                        picoquic_packet_context_t *pc = (picoquic_packet_context_t *) get_path(udtmp->path, AK_PATH_PKT_CTX, picoquic_packet_context_application);
                        picoquic_sack_item_t* first_sack = (picoquic_sack_item_t *) get_pkt_ctx(pc, AK_PKTCTX_FIRST_SACK_ITEM);
                        uint64_t first_sack_start_range = (uint64_t) get_sack_item(first_sack, AK_SACKITEM_START_RANGE);
                        if (first_sack_start_range != (uint64_t)((int64_t)-1)) {  // Don't reserve for receiving uniflow without activity
                            reserve_mp_ack_frame(cnx, udtmp->path, picoquic_packet_context_application);
                            /* Consider here that the ping have been processed */
                            set_path(udtmp->path, AK_PATH_PING_RECEIVED, 0, 0);
                        }
                    }
                }

                size_t queued_bytes = 0;
                size_t consumed = 0;
                queue_t *reserved_frames = (queue_t *) get_cnx(cnx, AK_CNX_RESERVED_FRAMES, 0);
                if (queue_peek(reserved_frames) == NULL) {
                    stream = helper_schedule_next_stream(cnx, send_buffer_min_max - checksum_overhead - length, sending_path);
                    picoquic_frame_fair_reserve(cnx, sending_path, stream, send_buffer_min_max - checksum_overhead - length);
                }

                do { // Enqueue and write frame until no more bytes can be queued or written
                    queue_t *reserved_frames = (queue_t *) get_cnx(cnx, AK_CNX_RESERVED_FRAMES, 0);
                    if (queue_peek(reserved_frames) == NULL) {
                        queued_bytes = picoquic_frame_fair_reserve(cnx, sending_path, stream, send_buffer_min_max - checksum_overhead - length);
                    }

                    consumed = 0;
                    unsigned int is_pure_ack = (unsigned int) get_pkt(packet, AK_PKT_IS_PURE_ACK);
                    ret = helper_scheduler_write_new_frames(cnx, &bytes[length], send_buffer_min_max - checksum_overhead - length, length - get_pkt(packet, AK_PKT_OFFSET), packet, &consumed, &is_pure_ack);

                    if (!ret && consumed > send_buffer_min_max - checksum_overhead - length) {
                        ret = PICOQUIC_ERROR_FRAME_BUFFER_TOO_SMALL;
                    } else if (ret == 0) {
                        length += consumed;
                        set_pkt(packet, AK_PKT_IS_PURE_ACK, get_pkt(packet, AK_PKT_IS_PURE_ACK) && is_pure_ack);

                        if (path_validation_in_progress && !is_pure_ack) {
                            copy_for_duplication(cnx, bpfdd, packet, bytes, length, consumed, is_pure_ack, true);
                        }
                    }
                } while (ret == 0 && queued_bytes > 0 && consumed < send_buffer_min_max - checksum_overhead - length);

                if (ret == 0) {
                    /* FIXME: Sorry, I'm lazy, this could be easily fixed by making this a PO.
                        * This is needed by the way the cwin is now handled. */
                    if (helper_is_ack_needed(cnx, current_time, pc, path_0)) {
                        if (helper_prepare_ack_frame(cnx, current_time, pc, &bytes[length], send_buffer_min_max - checksum_overhead - length, &data_bytes) == 0) {
                            length += (uint32_t)data_bytes;
                        }
                    }

                    /* if present, send path response. This ensures we send it on the right path */
                    if (path_challenge_response_to_send) {
#define PICOQUIC_CHALLENGE_LENGTH 8
                        for (int i = 0; i < bpfd->nb_receiving_proposed; i++) {
                            if ((path_challenge_response_to_send & (1 << i)) && send_buffer_min_max - checksum_overhead - length >= PICOQUIC_CHALLENGE_LENGTH + 1) {
                                picoquic_path_t *receiving_path = bpfd->receiving_uniflows[i]->path;
                                /* This is not really clean, but it will work */
                                my_memset(&bytes[length], picoquic_frame_type_path_response, 1);
                                uint8_t *challenge_response = (uint8_t *) get_path(receiving_path, AK_PATH_CHALLENGE_RESPONSE, 0);
                                my_memcpy(&bytes[length+1], challenge_response, PICOQUIC_CHALLENGE_LENGTH);
                                set_path(receiving_path, AK_PATH_CHALLENGE_RESPONSE_TO_SEND, 0, 0);
                                length += PICOQUIC_CHALLENGE_LENGTH + 1;
                                set_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED, 1);
                            }
                        }
                    }

                    if (cwin > bytes_in_transit) {
                        /* if present, send tls data */
                        if (tls_ready) {
                            ret = helper_prepare_crypto_hs_frame(cnx, 3, &bytes[length],
                                                                 send_buffer_min_max - checksum_overhead - length, &data_bytes);

                            if (ret == 0) {
                                length += (uint32_t)data_bytes;
                                if (data_bytes > 0)
                                {
                                    set_pkt(packet, AK_PKT_IS_PURE_ACK, 0);
                                    set_pkt(packet, AK_PKT_CONTAINS_CRYPTO, 1);
                                    set_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED, 1);

                                    if (path_validation_in_progress) {
                                        copy_for_duplication(cnx, bpfdd, packet, bytes, length, data_bytes, false, true);
                                    }
                                }
                            }
                        }

                        if (handshake_done_to_send) {
                            ret = helper_prepare_handshake_done_frame(cnx, bytes + length, send_buffer_min_max - checksum_overhead - length, &data_bytes);
                            if (ret == 0 && data_bytes > 0) {
                                length += (uint32_t) data_bytes;
                                set_pkt(packet, AK_PKT_HAS_HANDSHAKE_DONE, 1);
                                set_pkt(packet, AK_PKT_IS_PURE_ACK, 0);
                            }
                        }

                        /* If necessary, encode the max data frame */
                        uint64_t data_received = get_cnx(cnx, AK_CNX_DATA_RECEIVED, 0);
                        uint64_t maxdata_local = get_cnx(cnx, AK_CNX_MAXDATA_LOCAL, 0);
                        if (ret == 0 && 2 * data_received > maxdata_local) {
                            ret = helper_prepare_max_data_frame(cnx, 2 * data_received, &bytes[length],
                                                                send_buffer_min_max - checksum_overhead - length, &data_bytes);

                            if (ret == 0) {
                                length += (uint32_t)data_bytes;
                                if (data_bytes > 0) {
                                    copy_for_duplication(cnx, bpfdd, packet, bytes, length, data_bytes, false, true);
                                }
                            }
                            else if (ret == PICOQUIC_ERROR_FRAME_BUFFER_TOO_SMALL) {
                                ret = 0;
                            }
                        }
                        /* If necessary, encode the max stream data frames */
                        if (ret == 0) {
                            ret = helper_prepare_required_max_stream_data_frames(cnx, &bytes[length],
                                                                                 send_buffer_min_max - checksum_overhead - length, &data_bytes);
                            if (ret == 0) {
                                length += (uint32_t)data_bytes;
                                if (data_bytes > 0) {
                                    copy_for_duplication(cnx, bpfdd, packet, bytes, length, data_bytes, false, true);
                                }
                            }
                        }


                        /* If required, request for plugins */
                        uint8_t plugin_requested = (uint8_t) get_cnx(cnx, AK_CNX_PLUGIN_REQUESTED, 0);
                        if (ret == 0 && !plugin_requested) {
                            int is_retransmittable = 1;
                            uint16_t pids_to_request_size = (uint16_t) get_cnx(cnx, AK_CNX_PIDS_TO_REQUEST_SIZE, 0);
                            plugin_req_pid_t *pid_to_request;
                            uint64_t pid_id;
                            char *plugin_name;

                            for (int i = 0; ret == 0 && i < pids_to_request_size; i++) {
                                pid_to_request = (plugin_req_pid_t *) get_cnx(cnx, AK_CNX_PIDS_TO_REQUEST, i);
                                pid_id = (uint64_t) get_preq(pid_to_request, AK_PIDREQ_PID_ID);
                                plugin_name = (char *) get_preq(pid_to_request, AK_PIDREQ_PLUGIN_NAME);
                                ret = helper_write_plugin_validate_frame(cnx, &bytes[length], &bytes[send_buffer_min_max - checksum_overhead],
                                    pid_id, plugin_name, &data_bytes, &is_retransmittable);
                                if (ret == 0) {
                                    length += (uint32_t)data_bytes;
                                    if (data_bytes > 0)
                                    {
                                        set_pkt(packet, AK_PKT_IS_PURE_ACK, 0);
                                        set_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED, 1);
                                        set_preq(pid_to_request, AK_PIDREQ_REQUESTED, 1);

                                        if (path_validation_in_progress) {
                                            copy_for_duplication(cnx, bpfdd, packet, bytes, length, data_bytes, false, true);
                                        }
                                    }
                                }
                                else if (ret == PICOQUIC_ERROR_FRAME_BUFFER_TOO_SMALL) {
                                    ret = 0;
                                }
                            }
                            set_cnx(cnx, AK_CNX_PLUGIN_REQUESTED, 0, 1);
                        }

                        /* Encode the plugin frame, or frames */
                        while (plugin_stream != NULL) {
                            size_t stream_bytes_max = helper_stream_bytes_max(cnx, send_buffer_min_max - checksum_overhead - length, header_length, bytes);
                            ret = helper_prepare_plugin_frame(cnx, plugin_stream, &bytes[length],
                                                                stream_bytes_max, &data_bytes);
                            if (ret == 0) {
                                length += (uint32_t)data_bytes;
                                if (data_bytes > 0)
                                {
                                    set_pkt(packet, AK_PKT_IS_PURE_ACK, 0);
                                    set_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED, 1);

                                    if (path_validation_in_progress) {
                                        copy_for_duplication(cnx, bpfdd, packet, bytes, length, data_bytes, false, true);
                                    }
                                }

                                if (stream_bytes_max > checksum_overhead + length + 8) {
                                    plugin_stream = helper_find_ready_plugin_stream(cnx);
                                }
                                else {
                                    break;
                                }
                            }
                            else if (ret == PICOQUIC_ERROR_FRAME_BUFFER_TOO_SMALL) {
                                ret = 0;
                                break;
                            }
                        }

                        size_t stream_bytes_max = helper_stream_bytes_max(cnx, send_buffer_min_max - checksum_overhead - length, header_length, bytes);
                        stream = helper_schedule_next_stream(cnx, stream_bytes_max, sending_path);

                        /* Encode the stream frame, or frames */
                        while (stream != NULL) {
                            ret = helper_prepare_stream_frame(cnx, stream, &bytes[length],
                                                              stream_bytes_max, &data_bytes);
                            if (ret == 0) {
                                length += (uint32_t)data_bytes;
                                if (data_bytes > 0)
                                {
                                    set_pkt(packet, AK_PKT_IS_PURE_ACK, 0);
                                    set_pkt(packet, AK_PKT_IS_CONGESTION_CONTROLLED, 1);

                                    if (path_validation_in_progress && !helper_is_stream_frame_unlimited(bytes + length - data_bytes)) {
                                        copy_for_duplication(cnx, bpfdd, packet, bytes, length, data_bytes, false, true);
                                    }
                                }

                                if (stream_bytes_max > checksum_overhead + length + 8) {
                                    stream_bytes_max = helper_stream_bytes_max(cnx, send_buffer_min_max - checksum_overhead - length, header_length, bytes);
                                    stream = helper_schedule_next_stream(cnx, stream_bytes_max, sending_path);
                                } else {
                                    break;
                                }
                            } else if (ret == PICOQUIC_ERROR_FRAME_BUFFER_TOO_SMALL) {
                                ret = 0;
                                break;
                            }
                        }

                        if (length <= header_length) {
                            /* Mark the bandwidth estimation as application limited */
                            set_path(sending_path, AK_PATH_DELIVERED_LIMITED_INDEX, 0, get_path(sending_path, AK_PATH_DELIVERED, 0));
                        }
                    } else if ((void *) get_cnx(cnx, AK_CNX_CONGESTION_CONTROL_ALGORITHM, 0) != NULL) {
                        helper_congestion_algorithm_notify(cnx, sending_path, picoquic_congestion_notification_cwin_blocked, 0, 0, 0, current_time);
                    }
                    if (length == 0 || length == header_length) {
                        /* Don't flood the network with packets! */
                        PROTOOP_PRINTF(cnx, "Don't send packet of size 0 on sending path %p\n", (protoop_arg_t) sending_path);
                        length = 0;
                    } else if (length > 0 && length != header_length && length + checksum_overhead <= PICOQUIC_RESET_PACKET_MIN_SIZE) {
                        uint32_t pad_size = PICOQUIC_RESET_PACKET_MIN_SIZE - checksum_overhead - length + 1;
                        my_memset(&bytes[length], 0, pad_size);
                    }
                }
            }

        }
    }

    POP_LOG_CTX(cnx);

    set_cnx(cnx, AK_CNX_OUTPUT, 0, (protoop_arg_t) sending_path);
    set_cnx(cnx, AK_CNX_OUTPUT, 1, length);
    set_cnx(cnx, AK_CNX_OUTPUT, 2, header_length);

    return (protoop_arg_t) ret;
}