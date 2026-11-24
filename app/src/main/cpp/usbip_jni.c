#include <jni.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#define LOG_TAG "OpenUsbIp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define USBIP_VERSION 0x0111
#define OP_REQ_DEVLIST 0x8005
#define OP_REP_DEVLIST 0x0005
#define OP_REQ_IMPORT  0x8003
#define OP_REP_IMPORT  0x0003

#define USBIP_CMD_SUBMIT 0x00000001
#define USBIP_CMD_UNLINK 0x00000002
#define USBIP_RET_SUBMIT 0x00000003
#define USBIP_RET_UNLINK 0x00000004

static volatile int g_running = 0;
static int g_server_sock = -1;

static int recv_all(int sock, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(sock, p + got, len - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int send_all(int sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(sock, p + sent, len - sent, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

#pragma pack(push, 1)
typedef struct {
    char path[256];
    char busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bConfigurationValue;
    uint8_t bNumConfigurations;
    uint8_t bNumInterfaces;
} usbip_usb_device;

typedef struct {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
} usbip_usb_interface;

typedef struct {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
} usbip_header_basic;

typedef struct {
    uint32_t transfer_flags;
    int32_t transfer_buffer_length;
    int32_t start_frame;
    int32_t number_of_packets;
    int32_t interval;
    uint8_t setup[8];
} usbip_header_cmd_submit;

typedef struct {
    int32_t status;
    int32_t actual_length;
    int32_t start_frame;
    int32_t number_of_packets;
    int32_t error_count;
    uint8_t padding[8];
} usbip_header_ret_submit;
#pragma pack(pop)

static void fill_device_info(usbip_usb_device *dev, uint16_t vid, uint16_t pid,
                             uint8_t cls, uint8_t sub, uint8_t proto, uint8_t num_intfs) {
    memset(dev, 0, sizeof(*dev));
    strncpy(dev->path, "/sys/devices/usb1/1-1", sizeof(dev->path) - 1);
    strncpy(dev->busid, "1-1", sizeof(dev->busid) - 1);
    dev->busnum = htonl(1);
    dev->devnum = htonl(2);
    dev->speed = htonl(3); // USB_SPEED_HIGH
    dev->idVendor = htons(vid);
    dev->idProduct = htons(pid);
    dev->bcdDevice = htons(0x0100);
    dev->bDeviceClass = cls;
    dev->bDeviceSubClass = sub;
    dev->bDeviceProtocol = proto;
    dev->bConfigurationValue = 1;
    dev->bNumConfigurations = 1;
    dev->bNumInterfaces = num_intfs;
}

static void handle_urb_loop(int client, int usb_fd) {
    int flag = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    LOGI("Loop de URBs iniciado (usb_fd=%d)", usb_fd);

    while (g_running) {
        usbip_header_basic hdr;
        if (recv_all(client, &hdr, sizeof(hdr)) < 0) break;

        uint32_t cmd = ntohl(hdr.command);
        uint32_t seqnum = ntohl(hdr.seqnum);
        uint32_t direction = ntohl(hdr.direction);
        uint32_t ep = ntohl(hdr.ep);

        if (cmd == USBIP_CMD_SUBMIT) {
            usbip_header_cmd_submit sub;
            if (recv_all(client, &sub, sizeof(sub)) < 0) break;

            int32_t len = (int32_t)ntohl((uint32_t)sub.transfer_buffer_length);
            int is_in = (ep == 0) ? ((sub.setup[0] & 0x80) != 0) : (direction == 1);

            uint16_t wLength = (uint16_t)(sub.setup[6] | (sub.setup[7] << 8));
            size_t alloc_len = (len > 0) ? (size_t)len : 0;
            if (ep == 0 && wLength > alloc_len) alloc_len = wLength;

            uint8_t *data = NULL;
            if (alloc_len > 0) {
                data = (uint8_t *)calloc(1, alloc_len);
                if (!data) break;
                if (!is_in && len > 0) {
                    if (recv_all(client, data, (size_t)len) < 0) {
                        free(data);
                        break;
                    }
                }
            }

            int ret = 0;
            int status = 0;
            int actual = 0;

            if (ep == 0) {
                uint8_t reqType = sub.setup[0];
                uint8_t req = sub.setup[1];

                // Evita -EBUSY no Android quando Windows envia SET_CONFIGURATION ou SET_INTERFACE
                if ((reqType == 0x00 && req == 0x09) || (reqType == 0x01 && req == 0x0B)) {
                    status = 0;
                    actual = 0;
                } else {
                    struct usbdevfs_ctrltransfer ctrl;
                    memset(&ctrl, 0, sizeof(ctrl));
                    ctrl.bRequestType = reqType;
                    ctrl.bRequest = req;
                    ctrl.wValue = (uint16_t)(sub.setup[2] | (sub.setup[3] << 8));
                    ctrl.wIndex = (uint16_t)(sub.setup[4] | (sub.setup[5] << 8));
                    ctrl.wLength = wLength;
                    ctrl.timeout = 5000;
                    ctrl.data = data;

                    ret = ioctl(usb_fd, USBDEVFS_CONTROL, &ctrl);
                    if (ret < 0) {
                        LOGE("CTRL err=%d reqType=0x%02x req=0x%02x wVal=0x%04x", errno, reqType, req, ctrl.wValue);
                        status = -errno;
                        actual = 0;
                    } else {
                        status = 0;
                        actual = (len > 0 && ret > len) ? len : ret;
                    }
                }
            } else {
                struct usbdevfs_bulktransfer bulk;
                memset(&bulk, 0, sizeof(bulk));
                bulk.ep = ep | (is_in ? 0x80 : 0x00);
                bulk.len = (unsigned int)len;
                bulk.timeout = 5000;
                bulk.data = data;

                ret = ioctl(usb_fd, USBDEVFS_BULK, &bulk);
                if (ret < 0) {
                    status = -errno;
                    actual = 0;
                } else {
                    status = 0;
                    actual = ret;
                }
            }

            usbip_header_basic rhdr;
            memset(&rhdr, 0, sizeof(rhdr));
            rhdr.command = htonl(USBIP_RET_SUBMIT);
            rhdr.seqnum = htonl(seqnum);

            usbip_header_ret_submit rsub;
            memset(&rsub, 0, sizeof(rsub));
            rsub.status = htonl((uint32_t)status);
            rsub.actual_length = htonl((uint32_t)actual);

            int err = send_all(client, &rhdr, sizeof(rhdr)) < 0 ||
                      send_all(client, &rsub, sizeof(rsub)) < 0;
            if (!err && is_in && actual > 0 && data) {
                err = send_all(client, data, (size_t)actual) < 0;
            }
            free(data);
            if (err) break;

        } else if (cmd == USBIP_CMD_UNLINK) {
            uint8_t unlink_body[28];
            if (recv_all(client, unlink_body, sizeof(unlink_body)) < 0) break;

            usbip_header_basic rhdr;
            memset(&rhdr, 0, sizeof(rhdr));
            rhdr.command = htonl(USBIP_RET_UNLINK);
            rhdr.seqnum = htonl(seqnum);

            uint8_t rbody[28];
            memset(rbody, 0, sizeof(rbody));
            if (send_all(client, &rhdr, sizeof(rhdr)) < 0 ||
                send_all(client, rbody, sizeof(rbody)) < 0) break;
        } else {
            break;
        }
    }
    LOGI("Loop de URBs encerrado");
}

JNIEXPORT void JNICALL
Java_com_openusbip_MainActivity_stopUsbIpServer(JNIEnv *env, jobject thiz) {
    g_running = 0;
    if (g_server_sock >= 0) {
        shutdown(g_server_sock, SHUT_RDWR);
        close(g_server_sock);
        g_server_sock = -1;
    }
}

JNIEXPORT jstring JNICALL
Java_com_openusbip_MainActivity_startUsbIpServer(
        JNIEnv *env, jobject thiz, jint usb_fd, jint port,
        jint vid, jint pid, jint cls, jint sub, jint proto,
        jintArray intfTriples) {

    g_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_sock < 0) return (*env)->NewStringUTF(env, "Erro ao criar socket TCP");

    int opt = 1;
    setsockopt(g_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(g_server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_server_sock);
        g_server_sock = -1;
        return (*env)->NewStringUTF(env, "Erro no bind da porta 3240");
    }

    if (listen(g_server_sock, 4) < 0) {
        close(g_server_sock);
        g_server_sock = -1;
        return (*env)->NewStringUTF(env, "Erro no listen");
    }

    jsize arrLen = (*env)->GetArrayLength(env, intfTriples);
    int num_intfs = arrLen / 3;
    jint *elems = (*env)->GetIntArrayElements(env, intfTriples, NULL);

    g_running = 1;

    while (g_running) {
        int client = accept(g_server_sock, NULL, NULL);
        if (client < 0) break;

        int flag = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        uint16_t ver_net, op_net;
        uint32_t status_net;
        if (recv_all(client, &ver_net, 2) < 0 ||
            recv_all(client, &op_net, 2) < 0 ||
            recv_all(client, &status_net, 4) < 0) {
            close(client);
            continue;
        }

        uint16_t op = ntohs(op_net);
        usbip_usb_device dev;
        fill_device_info(&dev, (uint16_t)vid, (uint16_t)pid,
                         (uint8_t)cls, (uint8_t)sub, (uint8_t)proto, (uint8_t)num_intfs);

        if (op == OP_REQ_DEVLIST) {
            uint16_t r_ver = htons(USBIP_VERSION);
            uint16_t r_op = htons(OP_REP_DEVLIST);
            uint32_t r_status = htonl(0);
            uint32_t r_ndevs = htonl(1);

            send_all(client, &r_ver, 2);
            send_all(client, &r_op, 2);
            send_all(client, &r_status, 4);
            send_all(client, &r_ndevs, 4);
            send_all(client, &dev, sizeof(dev));

            for (int i = 0; i < num_intfs; i++) {
                usbip_usb_interface ui;
                ui.bInterfaceClass = (uint8_t)elems[i * 3];
                ui.bInterfaceSubClass = (uint8_t)elems[i * 3 + 1];
                ui.bInterfaceProtocol = (uint8_t)elems[i * 3 + 2];
                ui.padding = 0;
                send_all(client, &ui, sizeof(ui));
            }
            close(client);

        } else if (op == OP_REQ_IMPORT) {
            char busid[32];
            if (recv_all(client, busid, 32) < 0) {
                close(client);
                continue;
            }
            uint16_t r_ver = htons(USBIP_VERSION);
            uint16_t r_op = htons(OP_REP_IMPORT);
            uint32_t r_status = htonl(0);

            send_all(client, &r_ver, 2);
            send_all(client, &r_op, 2);
            send_all(client, &r_status, 4);
            send_all(client, &dev, sizeof(dev));

            handle_urb_loop(client, usb_fd);
            close(client);
        } else {
            close(client);
        }
    }

    (*env)->ReleaseIntArrayElements(env, intfTriples, elems, JNI_ABORT);
    if (g_server_sock >= 0) {
        close(g_server_sock);
        g_server_sock = -1;
    }
    return (*env)->NewStringUTF(env, "Servidor encerrado");
}
