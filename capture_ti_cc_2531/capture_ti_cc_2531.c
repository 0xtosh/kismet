
#include "../config.h"

#include "ti_cc2531.h"

#include <libusb-1.0/libusb.h>

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "../capture_framework.h"

/* Unique instance data passed around by capframework */
typedef struct {
    libusb_context *libusb_ctx;
    libusb_device_handle *ticc2531_handle;

    unsigned int devno, busno;

    pthread_mutex_t usb_mutex;

    /* we don't want to do a channel query every data response, we just want to 
     * remember the last channel used */
    unsigned int channel;

    /*keep track of our errors so we can reset if needed*/
    unsigned char error_ctr;

    bool ready;

    kis_capture_handler_t *caph;
} local_ticc2531_t;

/* Most basic of channel definitions */
typedef struct {
    unsigned int channel;
} local_channel_t;

int ticc2531_set_channel(kis_capture_handler_t *caph, uint8_t channel) {
    /* printf("channel %u\n", channel); */
    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;

    int ret;
    uint8_t data;

    localticc2531->ready = false;

    data = channel & 0xFF;
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    ret = libusb_control_transfer(localticc2531->ticc2531_handle, TICC2531_DIR_OUT, TICC2531_SET_CHAN, 0x00, 0x00, &data, 1, TICC2531_TIMEOUT);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    if (ret < 0) {
        return ret;
    }
    data = (channel >> 8) & 0xFF;
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    ret = libusb_control_transfer(localticc2531->ticc2531_handle, TICC2531_DIR_OUT, TICC2531_SET_CHAN, 0x00, 0x01, &data, 1, TICC2531_TIMEOUT);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    if (ret < 0) {
        return ret;
    }
    localticc2531->ready = true;

    return ret;
}///mutex inside

int ticc2531_set_power(kis_capture_handler_t *caph,uint8_t power, int retries) {
    int ret;
    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    // set power
    ret = libusb_control_transfer(localticc2531->ticc2531_handle, TICC2531_DIR_OUT, TICC2531_SET_POWER, 0x00, power, NULL, 0, TICC2531_TIMEOUT);
    // get power until it is the same as configured in set_power
    int i;
    for (i = 0; i < retries; i++)
    {
        uint8_t data;
        ret = libusb_control_transfer(localticc2531->ticc2531_handle, 0xC0, TICC2531_GET_POWER, 0x00, 0x00, &data, 1, TICC2531_TIMEOUT);
        if (ret < 0) {
            pthread_mutex_unlock(&(localticc2531->usb_mutex));
            return ret;
        }
        if (data == power) {
            pthread_mutex_unlock(&(localticc2531->usb_mutex));
            return 0;
        }
    }
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    return ret;
}//mutex inside

int ticc2531_enter_promisc_mode(kis_capture_handler_t *caph) {
    int ret;
    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    ret = libusb_control_transfer(localticc2531->ticc2531_handle, TICC2531_DIR_OUT, TICC2531_SET_START, 0x00, 0x00, NULL, 0, TICC2531_TIMEOUT);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    return ret;
}//mutex inside

int ticc2531_receive_payload(kis_capture_handler_t *caph, uint8_t *rx_buf, size_t rx_max) {
    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;
    int actual_len, r;
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    r = libusb_bulk_transfer(localticc2531->ticc2531_handle, TICC2531_DATA_EP, rx_buf, rx_max, &actual_len, TICC2531_TIMEOUT);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    if(r == LIBUSB_ERROR_TIMEOUT) {
        localticc2531->error_ctr++;
        if(localticc2531->error_ctr >= 5)
            return r;
        else
            return 1;/*continue on for now*/
    }
        
    if (r < 0)
        return r;
    localticc2531->error_ctr = 0;/*we got something valid so reset*/
    return actual_len;
}//mutex inside

int probe_callback(kis_capture_handler_t *caph, uint32_t seqno, char *definition,
        char *msg, char **uuid, KismetExternal__Command *frame,
        cf_params_interface_t **ret_interface,
        cf_params_spectrum_t **ret_spectrum) {
   
    char *placeholder = NULL;
    int placeholder_len;
    char *interface;
    char errstr[STATUS_MAX];

    *ret_spectrum = NULL;
    *ret_interface = cf_params_interface_new();

    int x;
    int busno = -1, devno = -1;

    libusb_device **libusb_devs = NULL;
    ssize_t libusb_devices_cnt = 0;
    int r;

    int matched_device = 0;

    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;

    if ((placeholder_len = cf_parse_interface(&placeholder, definition)) <= 0) {
        snprintf(msg, STATUS_MAX, "Unable to find interface in definition"); 
        return 0;
    }

    interface = strndup(placeholder, placeholder_len);

    /* Look for the interface type */
    if (strstr(interface, "ticc2531") != interface) {
        free(interface);
        return 0;
    }

    /* Look for interface-bus-dev */
    x = sscanf(interface, "ticc2531-%d-%d", &busno, &devno);
    free(interface);

    /* If we don't have a valid busno/devno or malformed interface name */
    if (x != -1 && x != 2) {
        return 0;
    }
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    libusb_devices_cnt = libusb_get_device_list(localticc2531->libusb_ctx, &libusb_devs);

    if (libusb_devices_cnt < 0) {
        return 0;
    }

    for (ssize_t i = 0; i < libusb_devices_cnt; i++) {
        struct libusb_device_descriptor dev;

        r = libusb_get_device_descriptor(libusb_devs[i], &dev);

        if (r < 0) {
            continue;
        }

        if (dev.idVendor == TICC2531_USB_VENDOR && dev.idProduct == TICC2531_USB_PRODUCT) {
            if (busno >= 0) {
                if (busno == libusb_get_bus_number(libusb_devs[i]) &&
                        devno == libusb_get_device_address(libusb_devs[i])) {
                    matched_device = 1;
                    break;
                }
            } else {
                matched_device = 1;
                busno = libusb_get_bus_number(libusb_devs[i]);
                devno = libusb_get_device_address(libusb_devs[i]);
                break;
            }
        }
    }
    libusb_free_device_list(libusb_devs, 1);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));

    /* Make a spoofed, but consistent, UUID based on the adler32 of the interface name 
     * and the location in the bus */
    snprintf(errstr, STATUS_MAX, "%08X-0000-0000-0000-%06X%06X",
            adler32_csum((unsigned char *) "kismet_cap_ti_cc2531", 
                strlen("kismet_cap_ti_cc2531")) & 0xFFFFFFFF,
            busno, devno);
    *uuid = strdup(errstr);

    /* TI CC 2531 supports 27-29 */
    (*ret_interface)->channels = (char **) malloc(sizeof(char *) * 16);
    for (int i = 11; i < 27; i++) {
        char chstr[4];
        snprintf(chstr, 4, "%d", i);
        (*ret_interface)->channels[i - 11] = strdup(chstr);
    }

    (*ret_interface)->channels_len = 16;
    return 1;
}/////mutex inside

int list_callback(kis_capture_handler_t *caph, uint32_t seqno,
        char *msg, cf_params_list_interface_t ***interfaces) {
    /* Basic list of devices */
    typedef struct ticc2531_list {
        char *device;
        struct ticc2531_list *next;
    } ticc2531_list_t; 

    ticc2531_list_t *devs = NULL;
    size_t num_devs = 0;

    libusb_device **libusb_devs = NULL;
    ssize_t libusb_devices_cnt = 0;
    int r;

    char devname[32];

    unsigned int i;

    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    libusb_devices_cnt = libusb_get_device_list(localticc2531->libusb_ctx, &libusb_devs);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    if (libusb_devices_cnt < 0) {
        return 0;
    }
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    for (ssize_t i = 0; i < libusb_devices_cnt; i++) {
        struct libusb_device_descriptor dev;

        r = libusb_get_device_descriptor(libusb_devs[i], &dev);

        if (r < 0) {
            continue;
        }

        if (dev.idVendor == TICC2531_USB_VENDOR && dev.idProduct == TICC2531_USB_PRODUCT) {
            snprintf(devname, 32, "ticc2531-%u-%u",
                libusb_get_bus_number(libusb_devs[i]),
                libusb_get_device_address(libusb_devs[i]));

            ticc2531_list_t *d = (ticc2531_list_t *) malloc(sizeof(ticc2531_list_t));
            num_devs++;
            d->device = strdup(devname);
            d->next = devs;
            devs = d;
        }
    }
    libusb_free_device_list(libusb_devs, 1);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    if (num_devs == 0) {
        *interfaces = NULL;
        return 0;
    }

    *interfaces = 
        (cf_params_list_interface_t **) malloc(sizeof(cf_params_list_interface_t *) * num_devs);

    i = 0;

    while (devs != NULL) {
        ticc2531_list_t *td = devs->next;
        (*interfaces)[i] = (cf_params_list_interface_t *) malloc(sizeof(cf_params_list_interface_t));
        memset((*interfaces)[i], 0, sizeof(cf_params_list_interface_t));

        (*interfaces)[i]->interface = devs->device;
        (*interfaces)[i]->flags = NULL;
        (*interfaces)[i]->hardware = strdup("ticc2531");

        free(devs);
        devs = td;

        i++;
    }
    return num_devs;
}///mutex inside

int open_callback(kis_capture_handler_t *caph, uint32_t seqno, char *definition,
        char *msg, uint32_t *dlt, char **uuid, KismetExternal__Command *frame,
        cf_params_interface_t **ret_interface,
        cf_params_spectrum_t **ret_spectrum) {

    char *placeholder = NULL;
    int placeholder_len;
    char *interface;
    char errstr[STATUS_MAX];

    *ret_spectrum = NULL;
    *ret_interface = cf_params_interface_new();

    int x;
    int busno = -1, devno = -1;

    libusb_device **libusb_devs = NULL;
    libusb_device *matched_dev = NULL;
    ssize_t libusb_devices_cnt = 0;
    int r;

    int matched_device = 0;
    char cap_if[32];

    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;

    if ((placeholder_len = cf_parse_interface(&placeholder, definition)) <= 0) {
        snprintf(msg, STATUS_MAX, "Unable to find interface in definition"); 
        return 0;
    }

    interface = strndup(placeholder, placeholder_len);

    /* Look for the interface type */
    if (strstr(interface, "ticc2531") != interface) {
        snprintf(msg, STATUS_MAX, "Unable to find ti cc2531 interface"); 
        free(interface);
        return -1;
    }

    /* Look for interface-bus-dev */
    x = sscanf(interface, "ticc2531-%d-%d", &busno, &devno);

    free(interface);

    /* If we don't have a valid busno/devno or malformed interface name */
    if (x != -1 && x != 2) {
        snprintf(msg, STATUS_MAX, "Malformed ticc2531 interface, expected 'ticc2531' or "
                "'ticc2531-bus#-dev#'"); 
        return -1;
    }
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    libusb_devices_cnt = libusb_get_device_list(localticc2531->libusb_ctx, &libusb_devs);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    if (libusb_devices_cnt < 0) {
        snprintf(msg, STATUS_MAX, "Unable to iterate USB devices"); 
        return -1;
    }
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    for (ssize_t i = 0; i < libusb_devices_cnt; i++) {
        struct libusb_device_descriptor dev;

        r = libusb_get_device_descriptor(libusb_devs[i], &dev);

        if (r < 0) {
            continue;
        }

        if (dev.idVendor == TICC2531_USB_VENDOR && dev.idProduct == TICC2531_USB_PRODUCT) {
            if (busno >= 0) {
                if (busno == libusb_get_bus_number(libusb_devs[i]) &&
                        devno == libusb_get_device_address(libusb_devs[i])) {
                    matched_device = 1;
                    matched_dev = libusb_devs[i];
                    break;
                }
            } else {
                matched_device = 1;
                busno = libusb_get_bus_number(libusb_devs[i]);
                devno = libusb_get_device_address(libusb_devs[i]);
                matched_dev = libusb_devs[i];
                break;
            }
        }
    }

    if (!matched_device) {
        snprintf(msg, STATUS_MAX, "Unable to find ticc2531 USB device");
        return -1;
    }

    libusb_free_device_list(libusb_devs, 1);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));

    snprintf(cap_if, 32, "ticc2531-%u-%u", busno, devno);

    localticc2531->devno = devno;
    localticc2531->busno = busno;

    /* Make a spoofed, but consistent, UUID based on the adler32 of the interface name 
     * and the location in the bus */
    snprintf(errstr, STATUS_MAX, "%08X-0000-0000-0000-%06X%06X",
            adler32_csum((unsigned char *) "kismet_cap_ti_cc2531", 
                strlen("kismet_cap_ti_cc2531")) & 0xFFFFFFFF,
            busno, devno);
    *uuid = strdup(errstr);

    (*ret_interface)->capif = strdup(cap_if);
    (*ret_interface)->hardware = strdup("ticc2531");

    /* BTLE supports 27-29 */
    (*ret_interface)->channels = (char **) malloc(sizeof(char *) * 16);
    for (int i = 11; i < 27; i++) {
        char chstr[4];
        snprintf(chstr, 4, "%d", i);
        (*ret_interface)->channels[i - 11] = strdup(chstr);
    }

    (*ret_interface)->channels_len = 16;

    pthread_mutex_lock(&(localticc2531->usb_mutex));
    /* Try to open it */
    r = libusb_open(matched_dev, &localticc2531->ticc2531_handle);
    pthread_mutex_unlock(&(localticc2531->usb_mutex));
    if (r < 0) {
        snprintf(errstr, STATUS_MAX, "Unable to open ticc2531 USB interface: %s", 
                libusb_strerror((enum libusb_error) r));
        pthread_mutex_unlock(&(localticc2531->usb_mutex));
        return -1;
    }
    pthread_mutex_lock(&(localticc2531->usb_mutex));
    if(libusb_kernel_driver_active(localticc2531->ticc2531_handle, 0)) {
        r = libusb_detach_kernel_driver(localticc2531->ticc2531_handle, 0); // detach driver
        assert(r == 0);
    }

    /* Try to claim it */
    r = libusb_claim_interface(localticc2531->ticc2531_handle, 0);
    if (r < 0) {
        if (r == LIBUSB_ERROR_BUSY) {
            /* Try to detach the kernel driver */
            r = libusb_detach_kernel_driver(localticc2531->ticc2531_handle, 0);
            if (r < 0) {
                snprintf(errstr, STATUS_MAX, "Unable to open ticc2531 USB interface, and unable "
                        "to disconnect existing driver: %s", 
                        libusb_strerror((enum libusb_error) r));
                pthread_mutex_unlock(&(localticc2531->usb_mutex));
                return -1;
            }
        } else {
            snprintf(errstr, STATUS_MAX, "Unable to open ticc2531 USB interface: %s",
                    libusb_strerror((enum libusb_error) r));
            pthread_mutex_unlock(&(localticc2531->usb_mutex));
            return -1;
        }
    }
    r = libusb_set_configuration(localticc2531->ticc2531_handle, -1);
    assert(r < 0);

    // read ident
    uint8_t ident[32];
    int ret;
    ret = libusb_control_transfer(localticc2531->ticc2531_handle, TICC2531_DIR_IN, TICC2531_GET_IDENT, 0x00, 0x00, ident, sizeof(ident), TICC2531_TIMEOUT);
/*
    if (ret > 0)
    {
        printf("IDENT:");
        for (int i = 0; i < ret; i++)
            printf(" %02X", ident[i]);
        printf("\n");
    }
*/
    pthread_mutex_unlock(&(localticc2531->usb_mutex));

    ticc2531_set_power(caph,0x04, TICC2531_POWER_RETRIES);
    ticc2531_enter_promisc_mode(caph);
    return 1;
}///mutex inside

void *chantranslate_callback(kis_capture_handler_t *caph, char *chanstr) {
    local_channel_t *ret_localchan;
    unsigned int parsechan;
    char errstr[STATUS_MAX];

    if (sscanf(chanstr, "%u", &parsechan) != 1) {
        snprintf(errstr, STATUS_MAX, "1 unable to parse requested channel '%s'; ticc2531 channels "
                "are from 11 to 26", chanstr);
        cf_send_message(caph, errstr, MSGFLAG_INFO);
        return NULL;
    }

    if (parsechan > 26 || parsechan < 11) {
        snprintf(errstr, STATUS_MAX, "2 unable to parse requested channel '%u'; ticc2531 channels "
                "are from 11 to 26", parsechan);
        cf_send_message(caph, errstr, MSGFLAG_INFO);
        return NULL;
    }

    ret_localchan = (local_channel_t *) malloc(sizeof(local_channel_t));
    ret_localchan->channel = parsechan;
    return ret_localchan;
}///

int chancontrol_callback(kis_capture_handler_t *caph, uint32_t seqno, void *privchan,
        char *msg) {
    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;
    local_channel_t *channel = (local_channel_t *) privchan;
    int r;

    if (privchan == NULL) {
        return 0;
    }
    if(privchan != channel->channel)
        r = ticc2531_set_channel(caph, channel->channel);

    if (r < 0)
        return -1;

    localticc2531->channel = channel->channel;
   
    return 1;
}///

bool verify_packet(unsigned char *data, int len) {

    unsigned char payload[128];memset(payload,0x00,128);
    int pkt_len = data[1];
    if(pkt_len != (len-3)) {
        printf("packet length mismatch\n");
        return false;
    }
    //get the paylaod
    int p_ctr=0;
    for(int i=8;i<(len-2);i++) {
        payload[p_ctr] = data[i];p_ctr++;
    }
    int payload_len = data[7] - 0x02;
    if(p_ctr != payload_len) {
        printf("payload size mismatch\n");
        return false;
    }

    unsigned char fcs1 = data[len-2];
    unsigned char fcs2 = data[len-1];
//rssi is the signed value at fcs1
    int rssi = (fcs1 + (int)pow(2,7)) % (int)pow(2,8) - (int)pow(2,7) - 73;
    unsigned char crc_ok = fcs2 & (1 << 7);
    unsigned char corr = fcs2 & 0x7f;
    if(crc_ok > 0) {
        return true;
    }
    else
        return false;
}

/* Run a standard glib mainloop inside the capture thread */
void capture_thread(kis_capture_handler_t *caph) {
    local_ticc2531_t *localticc2531 = (local_ticc2531_t *) caph->userdata;
    char errstr[STATUS_MAX];
    uint8_t usb_buf[256];
    int buf_rx_len, r;
    while (1) {
        if (caph->spindown) {
            /* close usb */
            if (localticc2531->ticc2531_handle) {
                libusb_close(localticc2531->ticc2531_handle);
                localticc2531->ticc2531_handle = NULL;
            }

            break;
        }
if(localticc2531->ready)
{
        buf_rx_len = ticc2531_receive_payload(caph, usb_buf, 256);
        if (buf_rx_len < 0) {
            snprintf(errstr, STATUS_MAX, "TI CC 2531 interface 'ticc2531-%u-%u' closed "
                    "unexpectedly", localticc2531->busno, localticc2531->devno);
            cf_send_error(caph, 0, errstr);
            cf_handler_spindown(caph);
            break;
        }

        /* Skip runt packets caused by timeouts */
        if (buf_rx_len == 1)
            continue;

        //the devices look to report a 4 byte counter/heartbeat, skip it
        if(buf_rx_len <= 7)
            continue;

        //if(!verify_packet(usb_buf, buf_rx_len)) {
        //printf("invalid packet\n");continue;}

        /**/
        if (buf_rx_len > 1) {
            fprintf(stderr, "ti cc 2531 saw %d ", buf_rx_len);

            for (int bb = 0; bb < buf_rx_len; bb++) {
                fprintf(stderr, "%02X ", usb_buf[bb] & 0xFF);
            }
            fprintf(stderr, "\n");
        }
        /**/

        while (1) {
            struct timeval tv;

            gettimeofday(&tv, NULL);

            if ((r = cf_send_data(caph,
                            NULL, NULL, NULL,
                            tv,
                            0,
                            buf_rx_len, usb_buf)) < 0) {
                cf_send_error(caph, 0, "unable to send DATA frame");
                cf_handler_spindown(caph);
            } else if (r == 0) {
                cf_handler_wait_ringbuffer(caph);
                continue;
            } else {
                break;
            }
        }
}

    }

    cf_handler_spindown(caph);
}///

int main(int argc, char *argv[]) {
    local_ticc2531_t localticc2531 = {
        .libusb_ctx = NULL,
        .ticc2531_handle = NULL,
        .caph = NULL,
    };

    pthread_mutex_init(&(localticc2531.usb_mutex), NULL);

    kis_capture_handler_t *caph = cf_handler_init("ticc2531");
    int r;

    if (caph == NULL) {
        fprintf(stderr, "FATAL: Could not allocate basic handler data, your system "
                "is very low on RAM or something is wrong.\n");
        return -1;
    }

    r = libusb_init(&localticc2531.libusb_ctx);
    if (r < 0) {
        return -1;
    }

    libusb_set_debug(localticc2531.libusb_ctx, 3);

    localticc2531.caph = caph;

    /* Set the local data ptr */
    cf_handler_set_userdata(caph, &localticc2531);

    /* Set the callback for opening  */
    cf_handler_set_open_cb(caph, open_callback);

    /* Set the callback for probing an interface */
    cf_handler_set_probe_cb(caph, probe_callback);

    /* Set the list callback */
    cf_handler_set_listdevices_cb(caph, list_callback);

    /* Channel callbacks */
    cf_handler_set_chantranslate_cb(caph, chantranslate_callback);
    cf_handler_set_chancontrol_cb(caph, chancontrol_callback);

    /* Set the capture thread */
    cf_handler_set_capture_cb(caph, capture_thread);

    if (cf_handler_parse_opts(caph, argc, argv) < 1) {
        cf_print_help(caph, argv[0]);
        return -1;
    }

    /* Support remote capture by launching the remote loop */
    cf_handler_remote_capture(caph);

    /* Jail our ns */
    cf_jail_filesystem(caph);

    /* Strip our privs */
    cf_drop_most_caps(caph);

    cf_handler_loop(caph);
    libusb_exit(localticc2531.libusb_ctx);

    return 0;
}
