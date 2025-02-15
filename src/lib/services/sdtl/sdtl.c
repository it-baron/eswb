#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "sdtl_opaque.h"
#include "bbee_framing.h"
#include "eswb/api.h"
#include "eswb/topic_proclaiming_tree.h"

static sdtl_rv_t ch_state_set_rx(sdtl_channel_handle_t *chh, sdtl_rx_state_t rx_state, sdtl_seq_code_t code);
static sdtl_rv_t ch_state_read(sdtl_channel_handle_t *chh, sdtl_channel_state_t *rx_state);
static sdtl_rv_t ch_state_alter_cond_flags(sdtl_channel_handle_t *chh, uint8_t cond_flags, int set);
static sdtl_rv_t ch_state_return_condition(sdtl_channel_handle_t *chh);

static sdtl_rv_t send_ack(sdtl_channel_handle_t *chh, uint8_t pkt_cnt, sdtl_ack_code_t ack_code);

void sdtl_debug_msg(const char *fn, const char *txt, ...) {
    va_list (args);
    char pn[16];
    pthread_getname_np(pthread_self(), pn, sizeof(pn));
    fprintf(stdout, "%s | %s | ", pn, fn);
    va_start (args,txt);
    vfprintf(stdout, txt, args);
    va_end (args);
    fprintf(stdout, "\n");
}


sdtl_channel_t *resolve_channel_by_id(sdtl_service_t *s, uint8_t ch_id) {

    for (unsigned i = 0; i < s->channels_num; i++) {
        if (s->channels[i].cfg.id == ch_id) {
            return &s->channels[i];
        }
    }

    return NULL;
}

sdtl_channel_handle_t *resolve_channel_handle_by_id(sdtl_service_t *s, uint8_t ch_id) {

    for (size_t i = 0; i < s->channels_num; i++) {
        if (s->channel_handles[i]->channel->cfg.id == ch_id) {
            return s->channel_handles[i];
        }
    }

    return NULL;
}

sdtl_channel_t *resolve_channel_by_name(sdtl_service_t *s, const char *name) {

    for (unsigned i = 0; i < s->channels_num; i++) {
        if (strcmp(s->channels[i].cfg.name, name) == 0) {
            return &s->channels[i];
        }
    }

    return NULL;
}

static int check_rel(sdtl_channel_handle_t *ch) {
    return (ch->channel->cfg.type == SDTL_CHANNEL_RELIABLE);
}


void *sdtl_alloc(size_t s) {
    return calloc(1, s);
}

static sdtl_rv_t process_data(sdtl_channel_handle_t *chh, sdtl_data_sub_header_t *data_header) {
    eswb_rv_t erv = eswb_fifo_push(chh->data_td, data_header);
    return erv == eswb_e_ok ? SDTL_OK : SDTL_ESWB_ERR;
}

static sdtl_rv_t rx_process_ack(sdtl_channel_handle_t *chh, sdtl_ack_sub_header_t *ack_header) {
    eswb_rv_t erv = eswb_fifo_push(chh->ack_td, ack_header);
    return erv == eswb_e_ok ? SDTL_OK : SDTL_ESWB_ERR;
}

static sdtl_rv_t validate_base_header(sdtl_base_header_t *hdr, uint8_t pkt_type, size_t data_len) {
    switch (pkt_type) {
        case SDTL_PKT_ATTR_PKT_TYPE_DATA:
            ; sdtl_data_header_t *dhdr = (sdtl_data_header_t *) hdr;
            if (dhdr->sub.payload_size != data_len - sizeof(*dhdr)) {
                return SDTL_NON_CONSIST_FRM_LEN;
            }
            break;

        case SDTL_PKT_ATTR_PKT_TYPE_ACK:
            ; sdtl_ack_header_t *ahdr = (sdtl_ack_header_t *) hdr;
            if (data_len != sizeof(*ahdr)) {
                return SDTL_NON_CONSIST_FRM_LEN;
            }
            break;

        case SDTL_PKT_ATTR_PKT_TYPE_CMD:
            ; sdtl_cmd_header_t *chdr = (sdtl_cmd_header_t *) hdr;
            if (data_len != sizeof(*chdr)) {
                return SDTL_NON_CONSIST_FRM_LEN;
            }
            break;

        default:
            return SDTL_INVALID_FRAME_TYPE;
    }

    return SDTL_OK;
}

static sdtl_rv_t
rx_process_data(sdtl_channel_handle_t *chh, sdtl_data_header_t *dhdr, sdtl_channel_state_t *rx_state) {
    sdtl_rv_t rv;

    do {
        if ((dhdr->sub.flags & (SDTL_PKT_DATA_FLAG_LAST_PKT)) &&
            (dhdr->sub.seq_code == rx_state->last_received_seq)) {
            sdtl_dbg_msg("Got rx state: ack trailing paket from prior seq 0x%04X", rx_state->last_received_seq);

            rv = send_ack(chh, dhdr->sub.cnt, SDTL_ACK_GOT_PKT);
            break;
        }

        switch (rx_state->rx_state) {
            default:
            case SDTL_RX_STATE_RCV_CANCELED:
                rv = send_ack(chh, dhdr->sub.cnt, SDTL_ACK_CANCELED);
                sdtl_dbg_msg("Got rx state: SDTL_RX_STATE_RCV_CANCELED");
                break;

            case SDTL_RX_STATE_SEQ_DONE:
            case SDTL_RX_STATE_IDLE:
                rv = send_ack(chh, dhdr->sub.cnt, SDTL_ACK_NO_RECEIVER);
                sdtl_dbg_msg("Got rx state: SDTL_RX_STATE_IDLE");
                break;

            case SDTL_RX_STATE_WAIT_DATA:
                rv = process_data(chh, &dhdr->sub);
                sdtl_dbg_msg("Got pkt #%d with %d bytes from ch_id #%d seq #%04X", dhdr->sub.cnt,
                             dhdr->sub.payload_size,
                             dhdr->base.ch_id,
                             dhdr->sub.seq_code);
                break;
        }
    } while(0);

    return rv;
}

static sdtl_rv_t rx_process_cmd(sdtl_channel_handle_t *chh, sdtl_cmd_header_t *chdr) {
    uint8_t flags = 0;
    sdtl_data_header_t *dhdr = (sdtl_data_header_t *) chdr;
    sdtl_ack_header_t *ahdr = (sdtl_ack_header_t *) chdr;

    //printf("%s: seq %d (last known seq code is %d)\n", __func__, chdr->cmd_seq_code, chh->rx_cmd_last_seq_code);

    if (chh->rx_cmd_last_seq_code != chdr->cmd_seq_code) {
        //printf("%s: seq %d processing\n", __func__, chdr->cmd_seq_code, chh->rx_cmd_last_seq_code);

        chh->rx_cmd_last_seq_code = chdr->cmd_seq_code;
        if (chdr->cmd_code == SDTL_PKT_CMD_CODE_RESET) {
            flags |= SDTL_CHANNEL_STATE_COND_FLAG_APP_RESET;
        }
        if (chdr->cmd_code == SDTL_PKT_CMD_CODE_CANCEL) {
            flags |= SDTL_CHANNEL_STATE_COND_FLAG_APP_CANCEL;
        }
        ch_state_alter_cond_flags(chh, flags, 1);
        // fake data pkt to unblock recipient fifo

        memset(&dhdr->sub, 0, sizeof(dhdr->sub));
        // convention is : dhdr->sub.payload_size == 0;
        process_data(chh, &dhdr->sub);

        // fake ack to unblock recipient fifo
        ahdr->sub.code = SDTL_ACK_OUT_BAND_EVENT;
        rx_process_ack(chh, &ahdr->sub);
    } else {
        // printf("%s: filtering out repetitive cmd with seq %d\n", __func__, chdr->cmd_seq_code);
    }

    return send_ack(chh, chh->rx_cmd_last_seq_code, SDTL_ACK_GOT_CMD);
}

static sdtl_rv_t sdtl_got_frame_handler (sdtl_service_t *s, uint8_t cmd_code, uint8_t *data, size_t data_len) {
    sdtl_rv_t rv;
    sdtl_base_header_t *base_header = (sdtl_base_header_t *) data;

    uint8_t pkt_type = SDTL_PKT_ATTR_PKT_GET_TYPE(base_header->attr);

    sdtl_channel_state_t rx_state;

    // validate packet
    rv = validate_base_header(base_header, pkt_type, data_len);
    if (rv != SDTL_OK) {
        // TODO send ack with error
        return rv;
    }

    // resolve channel
    sdtl_channel_handle_t *chh = resolve_channel_handle_by_id(s, base_header->ch_id);
    if (chh == NULL) {
        // TODO send ack with error
        return SDTL_NO_CHANNEL_LOCAL;
    }

    int rel = check_rel(chh);

    if (rel) {
        ch_state_read(chh, &rx_state);
    } else {
        rx_state.rx_state = SDTL_RX_STATE_WAIT_DATA;
    }

    sdtl_data_header_t *dhdr = (sdtl_data_header_t *) base_header;
    sdtl_ack_header_t *ahdr = (sdtl_ack_header_t *) base_header;
    sdtl_cmd_header_t *chdr = (sdtl_cmd_header_t *) base_header;

    switch (pkt_type) {
        case SDTL_PKT_ATTR_PKT_TYPE_DATA:
            rv = rx_process_data(chh, dhdr, &rx_state);
            break;


        case SDTL_PKT_ATTR_PKT_TYPE_ACK:
            rv = rx_process_ack(chh, &ahdr->sub);
            sdtl_dbg_msg("Got ack for pkt #%d", ahdr->sub.cnt);
            break;

        case SDTL_PKT_ATTR_PKT_TYPE_CMD:
            sdtl_dbg_msg("Got cmd code 0x%02X", chdr->cmd_code);
            if (rel) { ;
                rv = rx_process_cmd(chh, chdr);
            }
            break;

        default:
            // TODO throw invalid packet error
            sdtl_dbg_msg("Invalid packet");
            break;
    }

    return rv;
}


sdtl_rv_t bbee_frm_process_rx_buf(sdtl_service_t *s,
                                  uint8_t *rx_buf,
                                  size_t rx_buf_lng,
                                  bbee_frm_rx_state_t *rx_state,
                                  sdtl_rv_t (*rx_got_frame_handler)
                                   (sdtl_service_t *s, uint8_t cmd_code, uint8_t *data, size_t data_len)
                           ) {
    bbee_frm_rv_t frv;

    size_t bp = 0;
    size_t total_br = 0;

    rx_state->stat_processings++;

    do {
        frv = bbee_frm_rx_iteration(rx_state, rx_buf + total_br,
                                    rx_buf_lng - total_br, &bp);

        switch (frv) {
            default:
            case bbee_frm_ok:
                break;

            case bbee_frm_buf_overflow:
            case bbee_frm_inv_crc:
            case bbee_frm_got_empty_frame:
                switch (frv) {
                    case bbee_frm_buf_overflow:     rx_state->stat_buf_overflow++; break;
                    case bbee_frm_inv_crc:          rx_state->stat_inv_crc++; break;
                    case bbee_frm_got_empty_frame:  rx_state->stat_got_empty_frame++; break;
                    default: break;
                }
                bbee_frm_reset_state(rx_state);
                sdtl_dbg_msg("bbee_frm_rx_iteration reset event (%d)", frv);
                break;

            case bbee_frm_got_frame:
                rx_got_frame_handler(s, rx_state->current_command_code,
                                     rx_state->payload_buffer_origin,
                                     rx_state->current_payload_size);
//                        eqrb_dbg_msg("rx_got_frame_handler");
                bbee_frm_reset_state(rx_state);
                rx_state->stat_good_frames++;
                break;
        }

        if (frv != bbee_frm_ok) {
            sdtl_dbg_msg("Stat: CALLS=%05d GOOD=%05d RESTART=%05d INVCRC=%05d EMPTY=%05d OVFLW=%05d",
                         rx_state->stat_processings,
                         rx_state->stat_good_frames,
                         rx_state->stat_frame_restart,
                         rx_state->stat_inv_crc,
                         rx_state->stat_got_empty_frame,
                         rx_state->stat_buf_overflow);
        }

        total_br += bp;
    } while (total_br < rx_buf_lng);

    return SDTL_OK;
}

sdtl_rv_t bbee_frm_compose_frame(void *frame_buf, size_t frame_buf_size, void *hdr, size_t hdr_len, void *d, size_t d_len, size_t *frame_size_rv) {
    io_v_t iovec[3];

    iovec_set(iovec,0, hdr, hdr_len, 0);
    iovec_set(iovec,1, d, d_len, 1);

    bbee_frm_rv_t frv = bbee_frm_compose4tx_v(0, iovec, frame_buf, frame_buf_size, frame_size_rv);

    return frv == bbee_frm_ok ? SDTL_OK : SDTL_TX_BUF_SMALL;
}

sdtl_rv_t bbee_frm_allocate_tx_framebuf(size_t mtu, void **fb, size_t *l) {

//    if (available_buf_size < (payload_size * 2 /*for escape symbols*/ + 4 /*sync*/ + 2+2 /*crc and its escapes*/ + 1/*code*/)) {

    size_t fl = mtu * 2 + 10;
    *fb = sdtl_alloc(fl);
    if (*fb == NULL) {
        return SDTL_NO_MEM;
    }

    *l = fl;

    return SDTL_OK;
}

static void print_debug_data(uint8_t *rx_buf, size_t rx_buf_lng) {
//    char dbg_data[rx_buf_lng * 4];
//    char ss[4];
//    dbg_data[0] = 0;
//    for (int i = 0; i < rx_buf_lng; i++) {
//        sprintf(ss, "%02X ", rx_buf[i]);
//        strcat(dbg_data, ss);
//    }
//    eqrb_dbg_msg("recv | data | %s", dbg_data);
}

sdtl_rv_t sdtl_service_rx_thread(sdtl_service_t *s) {
    char thread_name[16];
#define SDTL_RX_THREAD_NAME_PREFIX "sdtl_rx_"
    strcpy(thread_name, SDTL_RX_THREAD_NAME_PREFIX);
    strncat(thread_name, s->service_name, sizeof(thread_name) - sizeof(SDTL_RX_THREAD_NAME_PREFIX) - 1);

    eswb_set_thread_name(thread_name);

    bbee_frm_rx_state_t rx_state;

    size_t payload_size = s->mtu + 10;
    size_t rx_buf_size = payload_size << 1; // * 2


    uint8_t *rx_buf = sdtl_alloc(rx_buf_size);
    uint8_t *payload_buf = sdtl_alloc(payload_size);

    bbee_frm_init_state(&rx_state, payload_buf, payload_size);

    sdtl_rv_t media_rv;

    int loop = -1;

    do {
        size_t rx_buf_lng;
        media_rv = s->media->read(s->media_handle, rx_buf, rx_buf_size, &rx_buf_lng);

        switch (media_rv) {
            case SDTL_OK:
                break;

            default:
            case SDTL_MEDIA_EOF:
                loop = 0;
                break;
        }

#       ifdef EQRB_DEBUG
        print_debug_data(rx_buf, rx_buf_lng);
#       endif

        bbee_frm_process_rx_buf(s, rx_buf, rx_buf_lng, &rx_state, sdtl_got_frame_handler);
        s->rx_stat.bytes_received += rx_buf_lng;
        s->rx_stat.non_framed_bytes = rx_state.stat_non_framed_bytes;
        s->rx_stat.frames_received = rx_state.stat_good_frames;
        s->rx_stat.bad_crc_frames = rx_state.stat_inv_crc;
        eswb_update_topic(s->rx_stat_td, &s->rx_stat);
    } while (loop);

    return SDTL_OK;
}


static sdtl_rv_t media_tx(sdtl_channel_handle_t *chh, void *header, uint32_t hl, void *d, uint32_t l) {
    size_t composed_frame_len;
    sdtl_rv_t rv = bbee_frm_compose_frame(chh->tx_frame_buf, chh->tx_frame_buf_size, header, hl, d, l, &composed_frame_len);
    if (rv != SDTL_OK) {
        return rv;
    }

    rv = chh->channel->service->media->write(chh->channel->service->media_handle, chh->tx_frame_buf, composed_frame_len);
    chh->tx_stat.bytes += composed_frame_len;

    return rv;
}



#define my_min(a,b) (((a) < (b)) ? (a) : (b))

static sdtl_rv_t
send_data(sdtl_channel_handle_t *chh, uint8_t pkt_num, uint8_t flags, sdtl_seq_code_t seq_code, void *d, uint32_t l) {
    sdtl_data_header_t hdr;

    hdr.base.attr = SDTL_PKT_ATTR_PKT_TYPE(SDTL_PKT_ATTR_PKT_TYPE_DATA);
    hdr.base.ch_id = chh->channel->cfg.id;

    hdr.sub.seq_code = seq_code;
    hdr.sub.cnt = pkt_num;
    hdr.sub.payload_size = l;
    hdr.sub.flags = flags;

    return media_tx(chh, &hdr, sizeof(hdr), d, l);
}

static sdtl_rv_t send_cmd(sdtl_channel_handle_t *chh, sdtl_seq_code_t seq_code, uint8_t cmd_code) {
    sdtl_cmd_header_t hdr;

    hdr.base.attr = SDTL_PKT_ATTR_PKT_TYPE(SDTL_PKT_ATTR_PKT_TYPE_CMD);
    hdr.base.ch_id = chh->channel->cfg.id;

    hdr.cmd_seq_code = seq_code;
    hdr.cmd_code = cmd_code;

    return media_tx(chh, &hdr, sizeof(hdr), NULL, 0);
}


static sdtl_rv_t send_ack(sdtl_channel_handle_t *chh, uint8_t pkt_cnt, sdtl_ack_code_t ack_code) {
    sdtl_ack_header_t hdr;

    hdr.base.attr = SDTL_PKT_ATTR_PKT_TYPE(SDTL_PKT_ATTR_PKT_TYPE_ACK);

    hdr.base.ch_id = chh->channel->cfg.id;

    hdr.sub.code = ack_code;
    hdr.sub.cnt = pkt_cnt;

    return media_tx(chh, &hdr, sizeof(hdr), NULL, 0);
}

static sdtl_rv_t
wait_ack_reset(sdtl_channel_handle_t *chh) {
    eswb_rv_t rv;
    rv = eswb_fifo_flush(chh->ack_td);
    return rv == eswb_e_ok ? SDTL_OK : SDTL_ESWB_ERR;
}


static sdtl_rv_t wait_ack(sdtl_channel_handle_t *chh, uint32_t timeout_us, sdtl_ack_sub_header_t *ack_sh_rv) {

    sdtl_ack_sub_header_t ack_sh;

    eswb_rv_t erv;
    sdtl_rv_t rv;
    if (timeout_us > 0) {
        eswb_arm_timeout(chh->ack_td, timeout_us);
    }
    erv = eswb_fifo_pop(chh->ack_td, &ack_sh);
    chh->armed_timeout_us = 0;

    switch (erv) {
        case eswb_e_ok:
            if (ack_sh.code == SDTL_ACK_OUT_BAND_EVENT) {
                rv = ch_state_return_condition(chh);
            } else {
                rv = SDTL_OK;
            }
            break;

        case eswb_e_timedout:
            rv = SDTL_TIMEDOUT;
            break;

        default:
            sdtl_dbg_msg("eswb_fifo_pop unhandled error: %s", eswb_strerror(erv));
            rv = SDTL_ESWB_ERR;
            break;
    }

    *ack_sh_rv = ack_sh;

    return rv;
}

static sdtl_rv_t
wait_data_reset(sdtl_channel_handle_t *chh) {
    eswb_rv_t rv;

    rv = eswb_fifo_flush(chh->data_td);

    return rv == eswb_e_ok ? SDTL_OK : SDTL_ESWB_ERR;
}

static sdtl_rv_t
wait_data(sdtl_channel_handle_t *chh, void *d, uint32_t l, sdtl_data_sub_header_t **dsh_rv, int prev_pkt_num) {
    eswb_rv_t rv;
    sdtl_data_sub_header_t *dsh = chh->rx_dafa_fifo_buf;

    if (chh->armed_timeout_us > 0) {
        eswb_arm_timeout(chh->data_td, chh->armed_timeout_us);
    }

    rv = eswb_fifo_pop(chh->data_td, dsh);

    *dsh_rv = dsh;

    switch (rv) {
        case eswb_e_ok:
            if (dsh->payload_size == 0) {
                // out of band event
                return ch_state_return_condition(chh);
            }
            if (prev_pkt_num != -1) {
                int dc = dsh->cnt - (prev_pkt_num % 256);
                if (dc < 0) {
                    dc += 256;
                }

                if (dc > 1) {
                    return SDTL_OK_MISSED_PKT_IN_SEQ;
                } else if (dc == 0) {
                    return SDTL_OK_REPEATED;
                } else {
                    if (l < dsh->payload_size) {
                        return SDTL_RX_BUF_SMALL;
                    }

                    // data is ok
                    return SDTL_OK;
                }
            } else {
                if (dsh->flags & SDTL_PKT_DATA_FLAG_FIRST_PKT) {
                    return SDTL_OK_FIRST_PACKET;
                } else {
                    return SDTL_OK_OMIT;
                }
            }

        case eswb_e_timedout:
            return SDTL_TIMEDOUT;

        case eswb_e_fifo_rcvr_underrun:
            chh->fifo_overflow++;
            // data is still good
            return SDTL_RX_FIFO_OVERFLOW;

        default:
            return SDTL_ESWB_ERR;
    }

    return SDTL_OK;
}


//static sdtl_rv_t update_tx_state(sdtl_channel_handle_t *chh, int pkt_cnt) {
//    // TODO make enumeration for transition, make transitions accordingly
//
//    // TODO
//
//    if (pkt_cnt > 0) {
//
//    }
//
//    return SDTL_OK;
//}

sdtl_seq_code_t generate_seq_code(unsigned seq_num) {
    sdtl_seq_code_t seq_code;
    struct timespec timespec;
    clock_gettime(CLOCK_MONOTONIC, &timespec);

    // pseudorandom seq num
    seq_code = seq_num + (timespec.tv_nsec >> 10);
    if (seq_code == 0) {
        seq_code--;
    }

    return seq_code;
}

// TODO timeout value must be calculated based on specivied interface speed (constant delay, e.g. proparation and data size related)
#define ACK_WAIT_TIMEOUT_uS_PER_BYTE(b__) ((80000) + (b__) * 8 * 1000000 / (57600 / 10))

static sdtl_rv_t channel_send_data(sdtl_channel_handle_t *chh, int rel, void *d, size_t l) {
    sdtl_pkt_payload_size_t dsize;
    uint32_t offset = 0;

    sdtl_rv_t rv = SDTL_OK;

    uint8_t pktn_in_seq = 0;
    uint8_t flags = SDTL_PKT_DATA_FLAG_FIRST_PKT |
                    (rel ? SDTL_PKT_DATA_FLAG_RELIABLE : 0);

    sdtl_seq_code_t seq_code = generate_seq_code(chh->tx_seq_num);

    sdtl_dbg_msg("================= call ================= (size == %d)", l);

    if (rel) {
        wait_ack_reset(chh);
    }

    chh->tx_stat.sequences++;

    do {
        dsize = my_min(chh->channel->max_payload_size, l);

        flags |= dsize == l ? SDTL_PKT_DATA_FLAG_LAST_PKT : 0;

        do {
            if (rel) {
                rv = ch_state_return_condition(chh);
                if (rv != SDTL_OK) {
                    sdtl_dbg_msg("Out of band event, return (on tranfer)");
                    return rv;
                }
            }
            rv = send_data(chh, pktn_in_seq, flags, seq_code, d + offset, dsize);
            sdtl_dbg_msg("Send %d bytes in pkt #%d in seq 0x%04X ch_id %d | rv == %d", dsize, pktn_in_seq, seq_code,
                         chh->channel->cfg.id,
                         rv);

            // TODO get timeout from somewhere
            if (rel) {
                sdtl_ack_sub_header_t ack_sh;
                rv = wait_ack(chh, ACK_WAIT_TIMEOUT_uS_PER_BYTE(dsize), &ack_sh);

                switch (rv) {
                    case SDTL_OK:
                        switch (ack_sh.code) {
                            case SDTL_ACK_GOT_PKT:
                                // TODO check pkt num
                                sdtl_dbg_msg("Got ack for pkt #%d", ack_sh.cnt);
                                break;

                            case SDTL_ACK_CANCELED:
                                sdtl_dbg_msg("Got ack with SDTL_ACK_CANCELED");
                                return SDTL_REMOTE_RX_CANCELED;

                            case SDTL_ACK_NO_RECEIVER:
                                sdtl_dbg_msg("Got ack with SDTL_ACK_NO_RECEIVER");
                                return SDTL_REMOTE_RX_NO_CLIENT;

                            default:
                                break;
                        }
                        break;

                    case SDTL_TIMEDOUT:
                        sdtl_dbg_msg("Ack timeout");
                        chh->tx_stat.retries++;
                        break;

                    case SDTL_APP_RESET:
                    case SDTL_APP_CANCEL:
                        sdtl_dbg_msg("Out of band event, return (on ack)");
                        return rv;

                    default:
                        // error
                        return rv;
                }
            }
        } while (rv == SDTL_TIMEDOUT);

        flags&= ~SDTL_PKT_DATA_FLAG_FIRST_PKT;

        pktn_in_seq++;

        l -= dsize;
        offset += dsize;

        chh->tx_stat.packets++;
        eswb_update_topic(chh->tx_stat_td, &chh->tx_stat);
    } while (l > 0);

    chh->tx_seq_num++;

    return rv;
}

static sdtl_rv_t channel_send_cmd(sdtl_channel_handle_t *chh, uint8_t cmd_code) {

    sdtl_rv_t rv;
    int loop = -1;
    // only for reliable channels
    if (!check_rel(chh)) {
        return SDTL_INVALID_CH_TYPE;
    }

    sdtl_seq_code_t seq_code = generate_seq_code(chh->tx_cmd_seq_num);

    sdtl_dbg_msg("================= call ================= (cmd == 0x%02)", cmd_code);

    sdtl_ack_sub_header_t ack_sh;
    do {
        rv = send_cmd(chh, seq_code, cmd_code);
        if (rv != SDTL_OK) {
            return rv;
        }

        rv = wait_ack(chh, ACK_WAIT_TIMEOUT_uS_PER_BYTE(20), &ack_sh);

        switch (rv) {
            case SDTL_APP_RESET:
            case SDTL_APP_CANCEL:
                loop = 0;
                rv = SDTL_OK;
                break;

            case SDTL_OK:
                if (ack_sh.code == SDTL_ACK_GOT_CMD) {
                    loop = 0;
                }
                break;

            case SDTL_TIMEDOUT:
                sdtl_dbg_msg("Cmd ack timeout");
                break;

            default:
                sdtl_dbg_msg("Cmd ack timeout");
                loop = 0;
                break;

        }

    } while (loop);

    chh->tx_cmd_seq_num++;

    return rv;
}

static sdtl_rv_t ch_state_set_rx(sdtl_channel_handle_t *chh, sdtl_rx_state_t rx_state, sdtl_seq_code_t code) {
    eswb_rv_t erv;
    sdtl_channel_state_t state;

    sdtl_rv_t rv = ch_state_read(chh, &state);
    if (rv != SDTL_OK) {
        return rv;
    }

    state.rx_state = rx_state;
    state.last_received_seq = code;

    erv = eswb_update_topic(chh->rx_state_td, &state);
    if (erv != eswb_e_ok) {
        return SDTL_ESWB_ERR;
    }

    return SDTL_OK;
}


static sdtl_rv_t ch_state_alter_cond_flags(sdtl_channel_handle_t *chh, uint8_t cond_flags, int set) {
    eswb_rv_t erv;
    sdtl_channel_state_t state;

    sdtl_rv_t rv = ch_state_read(chh, &state);
    if (rv != SDTL_OK) {
        return rv;
    }

    if (set) {
        state.condition_flags |= cond_flags;
    } else {
        state.condition_flags &= ~cond_flags;
    }

    erv = eswb_update_topic(chh->rx_state_td, &state);
    if (erv != eswb_e_ok) {
        return SDTL_ESWB_ERR;
    }

    return SDTL_OK;
}


static sdtl_rv_t ch_state_read(sdtl_channel_handle_t *chh, sdtl_channel_state_t *rx_state) {
    eswb_rv_t erv;
    erv = eswb_read(chh->rx_state_td, rx_state);
    if (erv != eswb_e_ok) {
        return SDTL_ESWB_ERR;
    }

    return SDTL_OK;
}

static sdtl_rv_t ch_state_return_condition(sdtl_channel_handle_t *chh) {
    sdtl_rv_t rv;

    // TODO create a mask to be able ignore some of the conditions

    sdtl_channel_state_t rx_state;

    rv = ch_state_read(chh, &rx_state);
    if (rv != SDTL_OK) {
        return rv;
    }

    if (rx_state.condition_flags & SDTL_CHANNEL_STATE_COND_FLAG_APP_RESET) {
        return SDTL_APP_RESET;
    } else if (rx_state.condition_flags & SDTL_CHANNEL_STATE_COND_FLAG_APP_CANCEL) {
        return SDTL_APP_CANCEL;
    }

    return SDTL_OK;
}

static sdtl_rv_t channel_recv_data(sdtl_channel_handle_t *chh, int rel, void *d, size_t s, size_t *br) {

    sdtl_rv_t rv_rcv;
    sdtl_rv_t rv_ack;
    sdtl_rv_t rv_state;
    sdtl_rv_t rv = SDTL_OK;

    sdtl_data_sub_header_t *dsh;

    // state variables:
    int sequence_started = 0;
    uint32_t offset = 0;
    size_t l = s;
    int prev_pkt_num = -1;

#   define RESET_SEQ()  l = s; \
                        offset = 0; \
                        prev_pkt_num = -1; \
                        sequence_started = 0

    int loop = -1;

    if (rel) {
        sdtl_channel_state_t rx_state;
        rv_state = ch_state_read(chh, &rx_state);
        if (rv_state != SDTL_OK) {
            return rv_state;
        }

        rv_state = ch_state_set_rx(chh, SDTL_RX_STATE_WAIT_DATA, rx_state.last_received_seq);
        if (rv_state != SDTL_OK) {
            return rv_state;
        }
        // TODO check state before, check that channal is not busy
    }

    sdtl_dbg_msg("================= call ================= (size == %d)", s);

#   define BREAK_LOOP(__rv) rv = __rv; loop = 0

    if (rel) {
        wait_data_reset(chh);
    }

    do {
        if (rel) {
            rv = ch_state_return_condition(chh);
            if (rv != SDTL_OK) {
                sdtl_dbg_msg("Out of band event, return (before waiting data)");
                break;
            }
        }
        rv_rcv = wait_data(chh, d + offset, l, &dsh, prev_pkt_num);

        switch (rv_rcv) {
            default:
                BREAK_LOOP(rv_rcv);
                break;

            case SDTL_APP_CANCEL:
            case SDTL_APP_RESET:
                BREAK_LOOP(rv_rcv);
                sdtl_dbg_msg("Out of band event, return (on wait_data)");
                break;

            case SDTL_RX_BUF_SMALL:
                BREAK_LOOP(rv_rcv);
                sdtl_dbg_msg("Buffer too small (l==%d against payload_size == %d)", l, dsh->payload_size);
                break;

            case SDTL_OK_REPEATED:
                if (rel) {
                    rv_ack = send_ack(chh, dsh->cnt, SDTL_ACK_GOT_PKT);
                    chh->rx_stat.acks++;
                }
                sdtl_dbg_msg("Got repeated pkt #%d", dsh->cnt);
                break;

            case SDTL_OK_OMIT:
                break;

            case SDTL_OK_FIRST_PACKET:
                chh->rx_stat.sequences++;

                sdtl_dbg_msg("Got SDTL_OK_FIRST_PACKET state");
                sequence_started = -1;
                prev_pkt_num = 0;
                // no break here
                // fall through

            case SDTL_OK:
                if (sequence_started) {
                    if (rel) {
                        rv_ack = send_ack(chh, dsh->cnt, SDTL_ACK_GOT_PKT);
                        chh->rx_stat.acks++;
                        if (rv_ack != SDTL_OK) {
                            sdtl_dbg_msg("Got send_ack err: %d", rv_ack);
                        }
                    } else {
                        // no action required
                    }

                    void *payload_data = ((void *) dsh) + sizeof(*dsh);
                    memcpy(d + offset, payload_data, dsh->payload_size);

                    offset += dsh->payload_size;
                    l -= dsh->payload_size;

                    chh->rx_stat.packets++;
                    chh->rx_stat.bytes += dsh->payload_size;

                    if (rv_rcv == SDTL_OK) {
                        prev_pkt_num++;
                    }

                    sdtl_dbg_msg("Recv %d bytes in pkt #%d in seq 0x%04X in ch_id %d",
                                 dsh->payload_size, dsh->cnt, dsh->seq_code, chh->channel->cfg.id);

                    if (dsh->flags & SDTL_PKT_DATA_FLAG_LAST_PKT) {
                        sdtl_dbg_msg("Got SDTL_PKT_DATA_FLAG_LAST_PKT flag");

                        rv = SDTL_OK;
                        loop = 0;
                    }
                }
                break;

            case SDTL_OK_MISSED_PKT_IN_SEQ:
                if (rel) {

                } else {
                    RESET_SEQ();
                }
                sdtl_dbg_msg("Got missed pkt in seq (pkt #%d, prev #%d)", dsh->cnt, prev_pkt_num);
                break;
        }
        eswb_update_topic(chh->rx_stat_td, &chh->rx_stat);
    } while (loop);

    // we want to keep timeout for sequentially arriving packets
    chh->armed_timeout_us = 0;

    if (rel) {
        if (rv == SDTL_OK) {
            ch_state_set_rx(chh, SDTL_RX_STATE_SEQ_DONE, dsh->seq_code);
        } else {
            ch_state_set_rx(chh, SDTL_RX_STATE_RCV_CANCELED, 0);
        }
    }

    if (rv == SDTL_OK) {
        *br = offset;
    }

    return rv;
}


#define CHANNEL_DATA_FIFO_NAME "data_fifo"
#define CHANNEL_ACK_FIFO_NAME "ack_fifo"

#define CHANNEL_DATA_BUF_NAME "data_buf"
#define CHANNEL_ACK_BUF_NAME "ack_buf"
#define CHANNEL_RX_STATE_STRUCT_NAME "rx_state"
#define CHANNEL_RX_STAT_NAME "rx_stat"
#define CHANNEL_TX_STAT_NAME "tx_stat"
//#define CHANNEL_RX_STATE_NAME "state"

#define CHANNEL_DATA_SUBPATH CHANNEL_DATA_FIFO_NAME "/" CHANNEL_DATA_BUF_NAME
#define CHANNEL_ACK_SUBPATH CHANNEL_ACK_FIFO_NAME "/" CHANNEL_ACK_BUF_NAME

static sdtl_rv_t check_channel_path(const char *root_path, const char *ch_name, const char *sub_path) {
    if (strlen(root_path) +
            strlen(ch_name) +
                strlen(sub_path) + 1 > ESWB_TOPIC_MAX_PATH_LEN) {
        return SDTL_NAMES_TOO_LONG;
    }

    return SDTL_OK;
}

static sdtl_rv_t check_channel_both_paths(const char *root_path, const char *ch_name) {

    if ((check_channel_path(root_path, ch_name, CHANNEL_DATA_SUBPATH) != SDTL_OK) ||
        (check_channel_path(root_path, ch_name, CHANNEL_ACK_SUBPATH) != SDTL_OK)) {
        return SDTL_NAMES_TOO_LONG;
    }

    return SDTL_OK;
}


static eswb_rv_t open_channel_resource(const char *base_path, const char* ch_name, const char *subpath, eswb_topic_descr_t *rv_td) {
    eswb_rv_t erv;

    char path[ESWB_TOPIC_MAX_PATH_LEN];

    strcpy(path, base_path);
    strcat(path, "/");
    strcat(path, ch_name);
    strcat(path, "/");
    strcat(path, subpath);

    return eswb_connect(path, rv_td);
}



#define SDTL_SERVICE_REG_RECORDS_MAX 4

// FIXME this is not thread safe construction:
static sdtl_service_t *sdtl_srv_reg[SDTL_SERVICE_REG_RECORDS_MAX];

static void srv_reg_add(sdtl_service_t *s) {
    for (int i = 0; i < SDTL_SERVICE_REG_RECORDS_MAX; i++) {
        if (sdtl_srv_reg[i] == NULL) {
            sdtl_srv_reg[i] = s;
            break;
        }
    }
}

static void srv_reg_delete(sdtl_service_t *s) {
    for (int i = 0; i < SDTL_SERVICE_REG_RECORDS_MAX; i++) {
        if (sdtl_srv_reg[i] == s) {
            sdtl_srv_reg[i] = NULL;
            break;
        }
    }
}

/*
 * FIXME we should not refer to service control structure in order to connect to it
 */
sdtl_service_t *sdtl_service_lookup(const char *service_name) {
    for (int i = 0; i < SDTL_SERVICE_REG_RECORDS_MAX; i++) {
        if ((sdtl_srv_reg[i] != NULL) && (strcmp(service_name, sdtl_srv_reg[i]->service_name) == 0)) {
            return sdtl_srv_reg[i];
        }
    }

    return NULL;
}


sdtl_rv_t sdtl_service_init(sdtl_service_t **s_rv, const char *service_name, const char *mount_point, size_t mtu,
                            size_t max_channels_num, const sdtl_service_media_t *media) {

    sdtl_service_t *s;
    s = sdtl_service_lookup(service_name);
    if (s != NULL) {
        *s_rv = s;
        return SDTL_SERVICE_EXIST;
    }

    if (media == NULL) {
        return SDTL_INVALID_MEDIA;
    }

    s = sdtl_alloc(sizeof(*s));
    if (s == NULL) {
        return SDTL_NO_MEM;
    }

    s->service_name = service_name;
    s->mtu = mtu == 0 ? SDTL_MTU_DEFAULT : mtu;
    s->max_channels_num = max_channels_num;
    s->media = media;
    s->channels = 0;

    s->service_eswb_root = sdtl_alloc(strlen(mount_point) + 1 + strlen(service_name) + 1);

    strcpy(s->service_eswb_root, mount_point);
    strcat(s->service_eswb_root, "/");
    strcat(s->service_eswb_root, s->service_name);

    s->channels = sdtl_alloc(max_channels_num * sizeof(*s->channels));
    if (s->channels == NULL) {
        return SDTL_NO_MEM;
    }

    s->channel_handles = sdtl_alloc(max_channels_num * sizeof(*s->channel_handles));
    if (s->channel_handles == NULL) {
        return SDTL_NO_MEM;
    }

    eswb_rv_t erv;
    erv = eswb_mkdir(mount_point, s->service_name);
    if (erv != eswb_e_ok) {
        sdtl_dbg_msg("eswb_mkdir failed for %s: %s", mount_point, eswb_strerror(erv));
        return SDTL_ESWB_ERR;
    }

    TOPIC_TREE_CONTEXT_LOCAL_DEFINE(cntx, 5);

    topic_proclaiming_tree_t *rx_stat_root = usr_topic_set_struct(cntx, s->rx_stat, "rx_stat");

    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_rx_stat_t, frames_received, "frames_received", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_rx_stat_t, bytes_received, "bytes_received", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_rx_stat_t, bad_crc_frames, "bad_crc_frames", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_rx_stat_t, non_framed_bytes, "non_framed_bytes", tt_uint32);

    erv = eswb_proclaim_tree_by_path(s->service_eswb_root, rx_stat_root, cntx->t_num, &s->rx_stat_td);
    if (erv != eswb_e_ok) {
        sdtl_dbg_msg("eswb_proclaim_tree_by_path failed: %s", eswb_strerror(erv));
    }

    *s_rv = s;

//    sdtl_dbg_msg("Success, media ref == 0x%016X", (uint64_t)s->media);

    return SDTL_OK;
}

sdtl_rv_t sdtl_service_init_w(sdtl_service_t **s_rv, const char *service_name, const char *mount_point, size_t mtu,
                            size_t max_channels_num, const char *media_name) {

    return sdtl_service_init(s_rv, service_name, mount_point, mtu, max_channels_num, sdtl_lookup_media(media_name));
}


sdtl_rv_t sdtl_service_start(sdtl_service_t *s, const char *media_path, void *media_params) {
    sdtl_rv_t rv;

//    sdtl_dbg_msg("Entered: path %s", media_path);

    rv = s->media->open(media_path, media_params, &s->media_handle);

    sdtl_dbg_msg("Media opened");

    if (rv != SDTL_OK) {
        return rv;
    }

    for (unsigned i = 0; i < s->channels_num; i++) {
        rv = sdtl_channel_open(s, s->channels[i].cfg.name, &s->channel_handles[i]);
        if (rv != SDTL_OK) {
            return rv;
        }
        if (check_rel(s->channel_handles[i])) {
            ch_state_set_rx(s->channel_handles[i], SDTL_RX_STATE_IDLE, 0);
        }
    }

    sdtl_dbg_msg("Channels inited");

    int prv = pthread_create(&s->rx_thread_tid, NULL, (void*)(void*) sdtl_service_rx_thread, s);
    if (prv) {
        return SDTL_SYS_ERR;
    }

    srv_reg_add(s);

    sdtl_dbg_msg("Success: \"%s\"", s->service_name);

    return SDTL_OK;
}

sdtl_rv_t sdtl_service_stop(sdtl_service_t *s) {
    int rv;

    rv = pthread_cancel(s->rx_thread_tid);
    if (rv != 0) {
        return SDTL_SYS_ERR;
    }

    rv = pthread_join(s->rx_thread_tid, NULL);
    if (rv != 0) {
        return SDTL_SYS_ERR;
    }

    for (unsigned i = 0; i < s->channels_num; i++) {
        sdtl_channel_close(s->channel_handles[i]);
    }

    srv_reg_delete(s);

    return SDTL_OK;
}


sdtl_rv_t sdtl_channel_create(sdtl_service_t *s, sdtl_channel_cfg_t *cfg) {

    if (resolve_channel_by_id(s, cfg->id)) {
        return SDTL_CH_EXIST;
    }

    if (s->channels_num >= s->max_channels_num) {
        return SDTL_NO_MEM;
    }

    sdtl_channel_t *ch = &s->channels[s->channels_num];

    memset(ch, 0, sizeof(*ch));

    size_t mtu = cfg->mtu_override > 0 ? my_min(s->mtu, cfg->mtu_override) : s->mtu;

    int max_payload_size = mtu - sizeof(sdtl_data_header_t);
    if (max_payload_size < 0) {
        return SDTL_INVALID_MTU;
    }

    ch->cfg = *cfg;
    ch->cfg.name = strdup(cfg->name); // otherwise we keep original name
    ch->service = s;
    ch->max_payload_size = max_payload_size;

    eswb_rv_t erv;

    erv = eswb_mkdir(s->service_eswb_root, cfg->name);
    if (erv != eswb_e_ok) {
        return SDTL_ESWB_ERR;
    }

    if (check_channel_both_paths(s->service_eswb_root, cfg->name) != SDTL_OK) {
        return SDTL_NAMES_TOO_LONG;
    }

    char path[ESWB_TOPIC_MAX_PATH_LEN + 1];

    strcpy(path, s->service_eswb_root);
    strcat(path, "/");
    strcat(path, cfg->name);

    // TODO fifo size supposed to be tuned according the speed of interface and overall service latency
#   define FIFO_SIZE 8

    TOPIC_TREE_CONTEXT_LOCAL_DEFINE(cntx,6);

    topic_proclaiming_tree_t *rx_stat_root = usr_topic_set_struct(cntx, sdtl_channel_rx_stat_t, CHANNEL_RX_STAT_NAME);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_channel_rx_stat_t, sequences, "sequences", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_channel_rx_stat_t, packets, "packets", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_channel_rx_stat_t, bytes, "bytes", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_channel_rx_stat_t, acks, "acks", tt_uint32);
    erv = eswb_proclaim_tree_by_path(path, rx_stat_root, cntx->t_num, NULL);
    if (erv != eswb_e_ok) {
        return SDTL_ESWB_ERR;
    }

    TOPIC_TREE_CONTEXT_LOCAL_RESET(cntx);
    topic_proclaiming_tree_t *tx_stat_root = usr_topic_set_struct(cntx, sdtl_channel_tx_stat_t, CHANNEL_TX_STAT_NAME);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_channel_tx_stat_t, sequences, "sequences", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_channel_tx_stat_t, packets, "packets", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_channel_tx_stat_t, bytes, "bytes", tt_uint32);
    usr_topic_add_struct_child(cntx, rx_stat_root, sdtl_channel_tx_stat_t, retries, "retries", tt_uint32);
    erv = eswb_proclaim_tree_by_path(path, tx_stat_root, cntx->t_num, NULL);
    if (erv != eswb_e_ok) {
        return SDTL_ESWB_ERR;
    }

    TOPIC_TREE_CONTEXT_LOCAL_RESET(cntx);
    topic_proclaiming_tree_t *data_fifo_root = usr_topic_set_fifo(cntx, CHANNEL_DATA_FIFO_NAME, FIFO_SIZE);
    usr_topic_add_child(cntx, data_fifo_root, CHANNEL_DATA_BUF_NAME, tt_plain_data, 0, mtu, 0);
    erv = eswb_proclaim_tree_by_path(path, data_fifo_root, cntx->t_num, NULL);
    if (erv != eswb_e_ok) {
        return SDTL_ESWB_ERR;
    }

    if (ch->cfg.type == SDTL_CHANNEL_RELIABLE) {
        TOPIC_TREE_CONTEXT_LOCAL_RESET(cntx);
        topic_proclaiming_tree_t *ack_fifo_root = usr_topic_set_fifo(cntx, CHANNEL_ACK_FIFO_NAME, FIFO_SIZE);
        usr_topic_add_child(cntx, ack_fifo_root, CHANNEL_ACK_BUF_NAME, tt_plain_data, 0, sizeof(sdtl_ack_sub_header_t),
                            0);
        erv = eswb_proclaim_tree_by_path(path, ack_fifo_root, cntx->t_num, NULL);
        if (erv != eswb_e_ok) {
            return SDTL_ESWB_ERR;
        }

        sdtl_channel_state_t rx_state;

        TOPIC_TREE_CONTEXT_LOCAL_RESET(cntx);
        topic_proclaiming_tree_t *rx_state_root = usr_topic_set_struct(cntx, rx_state, CHANNEL_RX_STATE_STRUCT_NAME);
        usr_topic_add_struct_child(cntx, rx_state_root, sdtl_channel_state_t, rx_state, "state", tt_uint32);
        erv = eswb_proclaim_tree_by_path(path, rx_state_root, cntx->t_num, NULL);
        if (erv != eswb_e_ok) {
            return SDTL_ESWB_ERR;
        }
    }

    s->channels_num++;

    sdtl_dbg_msg("Channel created: \"%s\", id: %d, type: %s", ch->cfg.name, ch->cfg.id,
                 ch->cfg.type == SDTL_CHANNEL_RELIABLE ? "REL" : "UNREL");

    return SDTL_OK;
}


sdtl_rv_t sdtl_channel_open(sdtl_service_t *s, const char *channel_name, sdtl_channel_handle_t **chh_rv) {

    if (s == NULL) {
        return SDTL_NO_SERVICE;
    }

    sdtl_channel_t *ch = resolve_channel_by_name(s, channel_name);
    if (ch == NULL) {
        return SDTL_NO_CHANNEL_LOCAL;
    }

    if (check_channel_both_paths(s->service_eswb_root, channel_name) != SDTL_OK) {
        return SDTL_NAMES_TOO_LONG;
    }

    sdtl_channel_handle_t *chh = sdtl_alloc(sizeof(*chh));
    if (chh == NULL) {
        return SDTL_NO_MEM;
    }

    chh->channel = ch;

    size_t mtu = s->mtu;

    sdtl_rv_t rv;
    rv = bbee_frm_allocate_tx_framebuf(mtu, &chh->tx_frame_buf, &chh->tx_frame_buf_size);
    if (rv != SDTL_OK) {
        return rv;
    }

    chh->rx_dafa_fifo_buf = sdtl_alloc(ch->max_payload_size + sizeof(sdtl_data_sub_header_t));
    if (chh->rx_dafa_fifo_buf == NULL) {
        return SDTL_NO_MEM;
    }

    eswb_rv_t erv;
    erv = open_channel_resource(s->service_eswb_root, channel_name, CHANNEL_DATA_FIFO_NAME, &chh->data_td);
    if (erv != eswb_e_ok) {
        sdtl_dbg_msg("open_channel_resource %s/%s error: %s", channel_name, CHANNEL_DATA_FIFO_NAME, eswb_strerror(erv));
        return SDTL_ESWB_ERR;
    }

    erv = open_channel_resource(s->service_eswb_root, channel_name, CHANNEL_RX_STAT_NAME, &chh->rx_stat_td);
    if (erv != eswb_e_ok) {
        sdtl_dbg_msg("open_channel_resource %s/%s error: %s", channel_name, CHANNEL_RX_STAT_NAME, eswb_strerror(erv));
        return SDTL_ESWB_ERR;
    }

    erv = open_channel_resource(s->service_eswb_root, channel_name, CHANNEL_TX_STAT_NAME, &chh->tx_stat_td);
    if (erv != eswb_e_ok) {
        sdtl_dbg_msg("open_channel_resource %s/%s error: %s", channel_name, CHANNEL_TX_STAT_NAME, eswb_strerror(erv));
        return SDTL_ESWB_ERR;
    }

    if (ch->cfg.type == SDTL_CHANNEL_RELIABLE) {
        erv = open_channel_resource(s->service_eswb_root, channel_name, CHANNEL_ACK_FIFO_NAME, &chh->ack_td);
        if (erv != eswb_e_ok) {
            sdtl_dbg_msg("open_channel_resource %w error: %s", CHANNEL_ACK_FIFO_NAME, eswb_strerror(erv));
            return SDTL_ESWB_ERR;
        }

        erv = open_channel_resource(s->service_eswb_root, channel_name, CHANNEL_RX_STATE_STRUCT_NAME, &chh->rx_state_td);
        if (erv != eswb_e_ok) {
            sdtl_dbg_msg("open_channel_resource %s error: %s", CHANNEL_RX_STATE_STRUCT_NAME, eswb_strerror(erv));
            return SDTL_ESWB_ERR;
        }
    }

    *chh_rv = chh;

    return SDTL_OK;
}

sdtl_rv_t sdtl_channel_close(sdtl_channel_handle_t *chh) {
    eswb_rv_t erv;

    // TODO free buffers
    // FIXME eswb_disconnect currently is not supported

    erv = eswb_disconnect(chh->data_td);
    if (erv != eswb_e_ok) {
        return SDTL_ESWB_ERR;
    }

    if (chh->channel->cfg.type == SDTL_CHANNEL_RELIABLE) {
        erv = eswb_disconnect(chh->ack_td);
        if (erv != eswb_e_ok) {
            return SDTL_ESWB_ERR;
        }
        erv = eswb_disconnect(chh->rx_state_td);
        if (erv != eswb_e_ok) {
            return SDTL_ESWB_ERR;
        }
    }

    // TODO free chh ?

    return SDTL_OK;
}


sdtl_rv_t sdtl_channel_recv_arm_timeout(sdtl_channel_handle_t *chh, uint32_t timeout_us) {
    // TODO limit timeout abs value
    chh->armed_timeout_us = timeout_us;
    return SDTL_OK;
}

sdtl_rv_t sdtl_channel_recv_data(sdtl_channel_handle_t *chh, void *d, uint32_t l, size_t *br) {
    int rel = check_rel(chh);
    return channel_recv_data(chh, rel, d, l, br);
}

sdtl_rv_t sdtl_channel_send_data(sdtl_channel_handle_t *chh, void *d, uint32_t l) {
    int rel = check_rel(chh);
    return channel_send_data(chh, rel, d, l);
}


sdtl_rv_t sdtl_channel_send_cmd(sdtl_channel_handle_t *chh, uint8_t code) {
    return channel_send_cmd(chh, code);
}

sdtl_rv_t sdtl_channel_check_reset_condition(sdtl_channel_handle_t *chh) {
    return ch_state_return_condition(chh);
}

sdtl_rv_t sdtl_channel_reset_condition(sdtl_channel_handle_t *chh) {
    return ch_state_alter_cond_flags(chh, 0xFF, 0);
}

uint32_t sdtl_channel_get_max_payload_size(sdtl_channel_handle_t *chh) {
    return chh->channel->max_payload_size;
}

const sdtl_service_media_t *sdtl_lookup_media(const char *mtype) {
    sdtl_dbg_msg("Look for \"%s\"", mtype);

    if (strcmp(mtype, "serial") == 0) {
        return &sdtl_media_serial;
    }

    return NULL;
}

const char *sdtl_strerror(sdtl_rv_t ecode) {
    switch (ecode) {
        case SDTL_OK:   return "SDTL_OK";
        case SDTL_TIMEDOUT: return "SDTL_TIMEDOUT";
        case SDTL_OK_FIRST_PACKET:  return "SDTL_OK_FIRST_PACKET";
        case SDTL_OK_OMIT:  return "SDTL_OK_OMIT";
        case SDTL_OK_REPEATED:  return "SDTL_OK_REPEATED";
        case SDTL_OK_MISSED_PKT_IN_SEQ: return "SDTL_OK_MISSED_PKT_IN_SEQ";
        case SDTL_REMOTE_RX_CANCELED:   return "SDTL_REMOTE_RX_CANCELED";
        case SDTL_REMOTE_RX_NO_CLIENT:  return "SDTL_REMOTE_RX_NO_CLIENT";
        case SDTL_RX_BUF_SMALL: return "SDTL_RX_BUF_SMALL";
        case SDTL_TX_BUF_SMALL: return "SDTL_TX_BUF_SMALL";
        case SDTL_NON_CONSIST_FRM_LEN:  return "SDTL_NON_CONSIST_FRM_LEN";
        case SDTL_INVALID_FRAME_TYPE:   return "SDTL_INVALID_FRAME_TYPE";
        case SDTL_NO_CHANNEL_REMOTE:    return "SDTL_NO_CHANNEL_REMOTE";
        case SDTL_NO_CHANNEL_LOCAL: return "SDTL_NO_CHANNEL_LOCAL";
        case SDTL_ESWB_ERR: return "SDTL_ESWB_ERR";
        case SDTL_RX_FIFO_OVERFLOW: return "SDTL_RX_FIFO_OVERFLOW";
        case SDTL_NO_MEM:   return "SDTL_NO_MEM";
        case SDTL_CH_EXIST: return "SDTL_CH_EXIST";
        case SDTL_SERVICE_EXIST:    return "SDTL_SERVICE_EXIST";
        case SDTL_NO_SERVICE:   return "SDTL_NO_SERVICE";
        case SDTL_INVALID_MTU:  return "SDTL_INVALID_MTU";
        case SDTL_INVALID_MEDIA:    return "SDTL_INVALID_MEDIA";
        case SDTL_NAMES_TOO_LONG:   return "SDTL_NAMES_TOO_LONG";
        case SDTL_SYS_ERR:  return "SDTL_SYS_ERR";
        case SDTL_INVALID_CH_TYPE:  return "SDTL_INVALID_CH_TYPE";
        case SDTL_MEDIA_NO_ENTITY:  return "SDTL_MEDIA_NO_ENTITY";
        case SDTL_MEDIA_NOT_SUPPORTED:  return "SDTL_MEDIA_NOT_SUPPORTED";
        case SDTL_MEDIA_ERR:    return "SDTL_MEDIA_ERR";
        case SDTL_MEDIA_EOF:    return "SDTL_MEDIA_EOF";
        case SDTL_APP_CANCEL:   return "SDTL_APP_CANCEL";
        case SDTL_APP_RESET:    return "SDTL_APP_RESET";
        default:
            return "Unhandled error code";
    }
}
