#include <eswb/services/sdtl.h>
#include <stdlib.h>
#include <string.h>
#include "../eqrb_priv.h"

typedef struct {
    const char *ch_name;
    const char *service_name;
} eqrb_drv_sdtl_params_t;

eqrb_rv_t eqrb_drv_sdtl_connect (void *param, device_descr_t *dh) {
    eqrb_drv_sdtl_params_t *p = (eqrb_drv_sdtl_params_t *)param;
    sdtl_channel_handle_t *chh;

    sdtl_service_t *service = sdtl_service_lookup(p->service_name);
    if (service == NULL){
        eqrb_dbg_msg("service \"%s\" not found", p->service_name);
    }

    sdtl_rv_t rv = sdtl_channel_open(service, p->ch_name, &chh);
    if (rv != SDTL_OK) {
        eqrb_dbg_msg("sdtl_channel_open failed: %d", rv);
    }

    *dh = chh;

    return rv == SDTL_OK ? eqrb_rv_ok : eqrb_media_err;
}

eqrb_rv_t eqrb_drv_sdtl_send (device_descr_t dh, void *data, size_t bts, size_t *bs) {

    sdtl_channel_handle_t *chh = (sdtl_channel_handle_t *) dh;

    sdtl_rv_t rv = sdtl_channel_send_data(chh, data, bts);

    switch (rv) {
        case SDTL_OK:
            *bs = bts;
            return eqrb_rv_ok;

        case SDTL_REMOTE_RX_CANCELED:
        case SDTL_REMOTE_RX_NO_CLIENT:
            return eqrb_media_remote_need_reset;

        case SDTL_APP_RESET:
            return eqrb_media_reset_cmd;

        default:
            return eqrb_media_err;
    }
}

eqrb_rv_t eqrb_drv_sdtl_recv (device_descr_t dh, void *data, size_t btr, size_t *br, uint32_t timeout) {
    sdtl_channel_handle_t *chh = (sdtl_channel_handle_t *) dh;

    sdtl_channel_recv_arm_timeout(chh, timeout);

    sdtl_rv_t rv = sdtl_channel_recv_data(chh, data, btr, br);
    switch (rv) {
        case SDTL_OK:
        case SDTL_RX_FIFO_OVERFLOW:
            return eqrb_rv_ok;

        case SDTL_APP_RESET:
            return eqrb_media_reset_cmd;

        case SDTL_TIMEDOUT:
            return eqrb_media_timedout;

        default:
            return eqrb_media_err;
    }
}

eqrb_rv_t eqrb_drv_sdtl_command (device_descr_t dh, eqrb_cmd_t cmd) {
    sdtl_channel_handle_t *chh = (sdtl_channel_handle_t *) dh;
    sdtl_rv_t rv;

    switch (cmd) {
        case eqrb_cmd_reset_remote:
            rv = sdtl_channel_send_cmd(chh, SDTL_PKT_CMD_CODE_RESET);
            break;

        case eqrb_cmd_reset_local_state:
            rv = sdtl_channel_reset_condition(chh);
            break;

        default:
            return eqrb_media_invarg;
    }

    if (rv != SDTL_OK) {
        eqrb_dbg_msg("failed: %d", rv);
    }

    return rv == SDTL_OK ? eqrb_rv_ok : eqrb_media_err;
}

eqrb_rv_t eqrb_drv_sdtl_check_state (device_descr_t dh) {
    sdtl_channel_handle_t *chh = (sdtl_channel_handle_t *) dh;
    sdtl_rv_t rv = sdtl_channel_check_reset_condition(chh);
    switch (rv) {
        case SDTL_APP_RESET:
        case SDTL_APP_CANCEL:
            return eqrb_media_reset_cmd;

        case SDTL_OK:
            return eqrb_rv_ok;

        default:
            return eqrb_media_err;
    }
}

eqrb_rv_t eqrb_drv_sdtl_disconnect (device_descr_t dh) {
//    return rv == SDTL_OK ? eqrb_rv_ok : eqrb_media_err;
    return eqrb_rv_ok;
}

const eqrb_media_driver_t eqrb_drv_sdtl = {
        .name = "eqrb_sdtl",
        .connect = eqrb_drv_sdtl_connect,
        .send = eqrb_drv_sdtl_send,
        .recv = eqrb_drv_sdtl_recv,
        .command = eqrb_drv_sdtl_command,
        .check_state = eqrb_drv_sdtl_check_state,
        .disconnect = eqrb_drv_sdtl_disconnect,
};

static eqrb_rv_t init_media_params(void **media_params, const char *service_name, const char *sdtl_ch_name) {

    eqrb_drv_sdtl_params_t *params = calloc(1, sizeof(*params));

    if (params == NULL) {
        return eqrb_rv_nomem;
    }

    params->service_name = strdup(service_name);
    params->ch_name = strdup(sdtl_ch_name);

    *media_params = params;

    return eqrb_rv_ok;
}

//
//static eqrb_rv_t
//instantiate_server(const char *service_name, const char *sdtl_ch_name, const char *bus2replicate, uint32_t ch_mask,
//                   int stream_only, const char **err_msg) {
//    eqrb_server_handle_t *sh = calloc(1, sizeof(*sh));
//
//    if (sh == NULL) {
//        return eqrb_rv_nomem;
//    }
//
//    eqrb_rv_t rv = init_handle(&sh->h, service_name, sdtl_ch_name);
//    if (rv != eqrb_rv_ok) {
//        return rv;
//    }
//
//    if (stream_only) {
//        sh->h.stream_only_mode = -1;
//    }
//
//    return eqrb_server_start(sh, bus2replicate, ch_mask, err_msg);
//}

eqrb_rv_t
eqrb_sdtl_server_start(const char *eqrb_service_name,
                       const char *sdtl_service_name, const char *sdtl_ch1_name, const char *sdtl_ch2_name, uint32_t ch_mask,
                       const char *bus2replicate, const char **err_msg) {

    void *mp;
    void *mp_sk = NULL;
    eqrb_rv_t rv;
    eqrb_server_handle_t *sh;

    rv = init_media_params(&mp, sdtl_service_name, sdtl_ch1_name);
    if (rv != eqrb_rv_ok) {
        return rv;
    }

    if (sdtl_ch2_name != NULL) {
        rv = init_media_params(&mp_sk, sdtl_service_name, sdtl_ch2_name);
        if (rv != eqrb_rv_ok) {
            return rv;
        }
    }

    eqrb_server_instance_init(eqrb_service_name,
                              &eqrb_drv_sdtl, mp, mp_sk, &sh);


    uint32_t ch_mask_main = (ch_mask & 0xFFFF) | 0x0001;
    uint32_t ch_mask_sk = ch_mask & 0xFFFF0000;

    if (sdtl_ch2_name != NULL && ch_mask_sk) {

    }

    return eqrb_server_start(sh, bus2replicate, ch_mask_main, ch_mask_sk, err_msg);
}


static eqrb_rv_t init_handle(eqrb_handle_common_t *h, const char *service_name, const char *sdtl_ch_name) {

    eqrb_drv_sdtl_params_t *params = calloc(1, sizeof(*params));

    if (params == NULL) {
        return eqrb_rv_nomem;
    }

    h->driver = &eqrb_drv_sdtl;
    h->connectivity_params = params;

    params->service_name = strdup(service_name);
    params->ch_name = strdup(sdtl_ch_name);

    return eqrb_rv_ok;
}

static eqrb_rv_t
instantiate_client(const char *service_name, const char *sdtl_ch_name, const char *mount_point, uint32_t repl_map_size,
                   int stream_only, eqrb_client_handle_t **sidekick_ch) {
    eqrb_client_handle_t *ch = calloc(1, sizeof(*ch));

    if (ch == NULL) {
        return eqrb_rv_nomem;
    }

    eqrb_rv_t rv = init_handle(&ch->h, service_name, sdtl_ch_name);
    if (rv != eqrb_rv_ok) {
        return rv;
    }

    if (stream_only) {
        // FIXME sidekick will use common ids_map in READ ONLY mode, but still it is not  thread safe
        // FIXME the whole design decision is VERY questionable
        ch->launch_sidekick = -1;
        ch->ids_map = (*sidekick_ch)->ids_map;
    } else {
        *sidekick_ch = ch;
    }

    return eqrb_client_start(ch, mount_point, repl_map_size);
}


eqrb_rv_t eqrb_sdtl_client_connect(const char *service_name, const char *sdtl_ch1_name, const char *sdtl_ch2_name,
                                   const char *mount_point, uint32_t repl_map_size) {

    eqrb_client_handle_t *ch;

    eqrb_rv_t rv = instantiate_client(service_name, sdtl_ch1_name, mount_point, repl_map_size, 0, &ch);
    if (rv != eqrb_rv_ok) {
        return rv;
    }

    if (sdtl_ch2_name != NULL) {
        rv = instantiate_client(service_name, sdtl_ch2_name, mount_point, repl_map_size, -1, &ch);
        if (rv != eqrb_rv_ok) {
            return rv;
        }
    }

    return eqrb_rv_ok;
}
