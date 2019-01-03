#include "picoquic_internal.h"
#include "bpf.h"

/**
 * cnx->protoop_inputv[0] = source_fpid_frame_t* frame
 * cnx->protoop_inputv[1] = const uint8_t* bytes_max
 * cnx->protoop_inputv[2] = uint64_t current_time
 *
 * Output: uint8_t* bytes
 */
protoop_arg_t process_source_fpid_frame(picoquic_cnx_t *cnx)
{
    source_fpid_frame_t *frame = (source_fpid_frame_t *) get_cnx(cnx, CNX_AK_INPUT, 0);
    PROTOOP_PRINTF(cnx, "PROCESS SFPID FRAME\n");
    bpf_state *state = get_bpf_state(cnx);
    uint8_t *payload = state->current_packet;
    if (payload){
        source_symbol_t *ss = malloc_source_symbol_with_data(cnx, frame->source_fpid, payload, state->current_packet_length);
        // the spin bit is always set to 0 and the spin_vec is always set to 1 in a source symbol to avoid differences between the source symbol and the real packet
        ss->data[0] &= 0xF8;
        ss->data[0] |= 0x01;
        if (!received_source_symbol_helper(cnx, state, ss)) {
            free_source_symbol(cnx, ss);
        }
    }
    return (protoop_arg_t) 0;
}