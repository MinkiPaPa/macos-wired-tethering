/*
 * usb_rndis.c
 * IOKit USB discovery, RNDIS control channel, bulk data pipes.
 * IOKit USB 탐색, RNDIS 제어 채널, 벌크 데이터 파이프.
 */
#include "usb_rndis.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOMessage.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <IOKit/usb/IOUSBHostFamilyDefinitions.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <sys/param.h>
#include <stdatomic.h>

typedef IOUSBDeviceInterface650 **at_dev_if_t;
typedef IOUSBInterfaceInterface650 **at_ifc_if_t;

struct at_usb_rndis {
    pthread_mutex_t mu;
    at_dev_if_t dev;
    at_ifc_if_t control_ifc;
    at_ifc_if_t data_ifc;
    io_service_t device_service;
    IONotificationPortRef notify_port;
    io_object_t interest;
    atomic_int gone;
    UInt8 control_num;
    UInt8 data_num;
    UInt8 bulk_in;
    UInt8 bulk_out;
    UInt8 interrupt_in;
    uint32_t xid;
    uint32_t max_xfer;
    uint32_t max_packets;
    uint32_t packet_align_shift;
    uint32_t bus_mbps;
    uint32_t rndis_link_mbps;
    uint32_t max_frame;
    int opened;

    CFRunLoopRef async_rl;
    CFRunLoopSourceRef async_src;
    at_usb_in_cb in_cb;
    void *in_user;
    at_usb_out_cb out_cb;
    void *out_user;
    struct {
        struct at_usb_rndis *u;
        uint8_t *buf;
        atomic_int busy;
    } in_urb[AT_USB_IN_DEPTH];
    struct {
        struct at_usb_rndis *u;
        uint8_t *buf;
        atomic_int busy;
    } out_urb;
};

/* ---------- registry helpers / 레지스트리 헬퍼 ---------- */

static int cfnum_u32(CFTypeRef v, uint32_t *out) {
    if (!v || !out) return -1;
    if (CFGetTypeID(v) != CFNumberGetTypeID()) return -1;
    return CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, out) ? 0 : -1;
}

static void cfstr_copy(CFTypeRef v, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return;
    CFStringGetCString((CFStringRef)v, out, (CFIndex)out_len, kCFStringEncodingUTF8);
}

static uint32_t prop_u32(io_registry_entry_t entry, const char *key) {
    CFStringRef ks = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    if (!ks) return 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(entry, ks, kCFAllocatorDefault, 0);
    CFRelease(ks);
    uint32_t n = 0;
    if (v) {
        cfnum_u32(v, &n);
        CFRelease(v);
    }
    return n;
}

static void prop_str(io_registry_entry_t entry, const char *key, char *out, size_t n) {
    CFStringRef ks = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    out[0] = '\0';
    if (!ks) return;
    CFTypeRef v = IORegistryEntryCreateCFProperty(entry, ks, kCFAllocatorDefault, 0);
    CFRelease(ks);
    if (v) {
        cfstr_copy(v, out, n);
        CFRelease(v);
    }
}

static int is_rndis_control(uint8_t cls, uint8_t sub, uint8_t proto) {
    /* Wireless Controller / Radio / RNDIS */
    if (cls == 0xE0 && sub == 0x01 && proto == 0x03) return 1;
    /* CDC ACM vendor-specific (Android USB tethering) */
    if (cls == 0x02 && sub == 0x02 && proto == 0xFF) return 1;
    /* Miscellaneous / RNDIS over Ethernet */
    if (cls == 0xEF && sub == 0x04 && proto == 0x01) return 1;
    return 0;
}

static int is_cdc_data(uint8_t cls, uint8_t sub, uint8_t proto) {
    (void)sub; (void)proto;
    return cls == 0x0A;
}

static int looks_android(uint8_t cls, uint8_t sub, uint8_t proto) {
    /* ADB: vendor-specific 0xFF / 0x42, or MTP still-image class 6. */
    /* ADB: 벤더 0xFF/0x42, 또는 MTP 스틸 이미지 클래스 6. */
    if (cls == 0xFF && proto == 0x42) return 1;
    if (cls == 0x06) return 1;
    if (cls == 0xFF && sub == 0x42) return 1;
    if (is_rndis_control(cls, sub, proto) || is_cdc_data(cls, sub, proto)) return 1;
    return 0;
}

static void inspect_interfaces(io_registry_entry_t device, at_usb_device_info_t *info) {
    io_iterator_t it = 0;
    if (IORegistryEntryCreateIterator(device, kIOServicePlane,
                                      kIORegistryIterateRecursively, &it) != KERN_SUCCESS) {
        return;
    }
    io_registry_entry_t child;
    int saw_android = 0;
    while ((child = IOIteratorNext(it)) != 0) {
        io_name_t cname;
        if (IOObjectGetClass(child, cname) == KERN_SUCCESS &&
            (strcmp(cname, "IOUSBHostInterface") == 0 ||
             strcmp(cname, "IOUSBInterface") == 0)) {
            uint8_t cls = (uint8_t)prop_u32(child, "bInterfaceClass");
            uint8_t sub = (uint8_t)prop_u32(child, "bInterfaceSubClass");
            uint8_t proto = (uint8_t)prop_u32(child, "bInterfaceProtocol");
            uint8_t num = (uint8_t)prop_u32(child, "bInterfaceNumber");
            if (looks_android(cls, sub, proto)) saw_android = 1;
            if (is_rndis_control(cls, sub, proto) && info->kind != AT_KIND_RNDIS) {
                info->kind = AT_KIND_RNDIS;
                info->control_ifc = num;
            }
            if (is_cdc_data(cls, sub, proto)) {
                info->data_ifc = num;
            }
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(it);
    if (info->kind != AT_KIND_RNDIS && saw_android) info->kind = AT_KIND_ANDROID;
}

static int looks_phone_name(const char *s);

static void fill_device_info(io_registry_entry_t device, at_usb_device_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->vid = (uint16_t)prop_u32(device, "idVendor");
    info->pid = (uint16_t)prop_u32(device, "idProduct");
    info->location_id = prop_u32(device, "locationID");
    prop_str(device, "USB Product Name", info->name, sizeof(info->name));
    if (info->name[0] == '\0') prop_str(device, "kUSBProductString", info->name, sizeof(info->name));
    prop_str(device, "USB Vendor Name", info->vendor, sizeof(info->vendor));
    if (info->vendor[0] == '\0') prop_str(device, "kUSBVendorString", info->vendor, sizeof(info->vendor));
    prop_str(device, "USB Serial Number", info->serial, sizeof(info->serial));
    if (info->serial[0] == '\0')
        prop_str(device, "kUSBSerialNumberString", info->serial, sizeof(info->serial));
    if (info->name[0] == '\0')
        snprintf(info->name, sizeof(info->name), "USB %04x:%04x", info->vid, info->pid);
    inspect_interfaces(device, info);
    if (info->kind == AT_KIND_UNKNOWN &&
        (looks_phone_name(info->name) || looks_phone_name(info->vendor))) {
        info->kind = AT_KIND_ANDROID;
    }
}

static io_iterator_t usb_device_iterator(void) {
    io_iterator_t it = 0;
    mach_port_t main_port = 0;
    if (IOMainPort(kIOMainPortDefault, &main_port) != KERN_SUCCESS) {
        main_port = kIOMainPortDefault;
    }
    CFMutableDictionaryRef matching = IOServiceMatching(kIOUSBHostDeviceClassName);
    if (!matching) matching = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matching) return 0;
    kern_return_t kr = IOServiceGetMatchingServices(main_port, matching, &it);
    if (kr != KERN_SUCCESS) {
        at_log(AT_LOG_ERROR, "IOServiceGetMatchingServices failed 0x%x", (unsigned)kr);
        return 0;
    }
    /* Zero matches can yield a NULL iterator on current macOS. That is not an error. */
    /* 매칭이 0건이면 최신 macOS는 NULL iterator를 줄 수 있다. 오류가 아니다. */
    return it;
}

static int looks_phone_name(const char *s) {
    if (!s || !s[0]) return 0;
    static const char *keys[] = {
        "samsung", "galaxy", "google", "pixel", "xiaomi", "redmi", "poco",
        "oneplus", "oppo", "vivo", "huawei", "honor", "sony", "xperia",
        "motorola", "nokia", "nothing", "asus", "rog phone", "lg ",
        "android", "phone", NULL
    };
    char lower[AT_MAX_NAME];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof(lower); i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        lower[i] = c;
    }
    lower[i] = '\0';
    for (int k = 0; keys[k]; k++) {
        if (strstr(lower, keys[k])) return 1;
    }
    return 0;
}

int at_usb_list_devices(at_usb_device_info_t *out, int max_out, int *count) {
    if (!out || max_out <= 0 || !count) return -1;
    *count = 0;
    io_iterator_t it = usb_device_iterator();
    if (!it) {
        /* No USB devices attached, or matching class currently empty. */
        /* USB 장치가 없거나 매칭 클래스가 비어 있다. */
        return 0;
    }
    io_service_t svc;
    while ((svc = IOIteratorNext(it)) != 0) {
        if (*count >= max_out) {
            IOObjectRelease(svc);
            break;
        }
        at_usb_device_info_t info;
        fill_device_info(svc, &info);
        IOObjectRelease(svc);
        if (info.vid == 0 && info.pid == 0) continue;
        /* Skip hubs / Apple internal devices without interesting classes. */
        /* 허브/Apple 내장 장치 중 관심 클래스가 없으면 건너뛴다. */
        if (info.kind == AT_KIND_UNKNOWN) continue;
        out[(*count)++] = info;
    }
    IOObjectRelease(it);
    return 0;
}

static io_service_t find_service(const at_usb_device_info_t *want) {
    io_iterator_t it = usb_device_iterator();
    if (!it) return 0;
    io_service_t found = 0;
    io_service_t svc;
    while ((svc = IOIteratorNext(it)) != 0) {
        at_usb_device_info_t info;
        fill_device_info(svc, &info);
        int match = 0;
        if (want->location_id != 0 && info.location_id == want->location_id) match = 1;
        else if (want->location_id == 0 && info.vid == want->vid && info.pid == want->pid) {
            if (want->serial[0] == '\0' || strcmp(want->serial, info.serial) == 0) match = 1;
        }
        if (match) {
            found = svc;
            break;
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return found;
}

static int plugin_device(io_service_t svc, at_dev_if_t *out) {
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) return -1;
    HRESULT hr = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID)out);
    (*plugin)->Release(plugin);
    return (hr == S_OK && *out) ? 0 : -1;
}

static int plugin_interface(io_service_t svc, at_ifc_if_t *out) {
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        svc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) return -1;
    HRESULT hr = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650), (LPVOID)out);
    (*plugin)->Release(plugin);
    return (hr == S_OK && *out) ? 0 : -1;
}

static void classify_pipes(at_ifc_if_t ifc, UInt8 *bulk_in, UInt8 *bulk_out, UInt8 *intr_in) {
    UInt8 n = 0;
    if ((*ifc)->GetNumEndpoints(ifc, &n) != kIOReturnSuccess) return;
    for (UInt8 i = 1; i <= n; i++) {
        UInt8 dir = 0, num = 0, xfer = 0, interval = 0;
        UInt16 mps = 0;
        if ((*ifc)->GetPipeProperties(ifc, i, &dir, &num, &xfer, &mps, &interval) != kIOReturnSuccess)
            continue;
        if (xfer == kUSBBulk && dir == kUSBIn && bulk_in && *bulk_in == 0) *bulk_in = i;
        if (xfer == kUSBBulk && dir == kUSBOut && bulk_out && *bulk_out == 0) *bulk_out = i;
        if (xfer == kUSBInterrupt && dir == kUSBIn && intr_in && *intr_in == 0) *intr_in = i;
    }
}

static int open_one_interface(at_dev_if_t dev, UInt8 want_num, at_ifc_if_t *out) {
    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    io_iterator_t it = 0;
    if ((*dev)->CreateInterfaceIterator(dev, &req, &it) != kIOReturnSuccess) return -1;
    io_service_t svc;
    int rc = -1;
    while ((svc = IOIteratorNext(it)) != 0) {
        at_ifc_if_t ifc = NULL;
        if (plugin_interface(svc, &ifc) == 0) {
            UInt8 num = 0;
            (*ifc)->GetInterfaceNumber(ifc, &num);
            if (num == want_num) {
                IOReturn orc = (*ifc)->USBInterfaceOpenSeize(ifc);
                if (orc != kIOReturnSuccess) orc = (*ifc)->USBInterfaceOpen(ifc);
                if (orc == kIOReturnSuccess) {
                    *out = ifc;
                    rc = 0;
                    IOObjectRelease(svc);
                    break;
                }
            }
            (*ifc)->Release(ifc);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return rc;
}

at_usb_rndis_t *at_usb_open(const at_usb_device_info_t *want) {
    if (!want) return NULL;
    io_service_t svc = find_service(want);
    if (!svc) {
        at_log(AT_LOG_ERROR, "USB device not found");
        return NULL;
    }
    at_usb_device_info_t live;
    fill_device_info(svc, &live);

    at_usb_rndis_t *u = calloc(1, sizeof(*u));
    if (!u) {
        IOObjectRelease(svc);
        return NULL;
    }
    pthread_mutex_init(&u->mu, NULL);
    atomic_init(&u->gone, 0);
    u->device_service = svc;
    u->max_xfer = AT_MAX_TRANSFER;
    u->max_packets = 1;
    u->packet_align_shift = 2;
    u->bus_mbps = 480;
    u->xid = 1;

    if (plugin_device(svc, &u->dev) != 0) {
        at_log(AT_LOG_ERROR, "Failed to create USB plugin");
        at_usb_close(u);
        return NULL;
    }

    IOReturn orc = (*u->dev)->USBDeviceOpenSeize(u->dev);
    if (orc != kIOReturnSuccess) orc = (*u->dev)->USBDeviceOpen(u->dev);
    if (orc != kIOReturnSuccess) {
        at_log(AT_LOG_WARN,
               "USBDeviceOpen failed (0x%08x); continuing with interfaces",
               (unsigned)orc);
    }

    /* Prefer live RNDIS interface numbers discovered from the registry. */
    /* 레지스트리에서 발견한 실제 RNDIS 인터페이스 번호를 우선 사용한다. */
    u->control_num = live.control_ifc;
    u->data_num = live.data_ifc;
    if (live.kind != AT_KIND_RNDIS) {
        at_log(AT_LOG_ERROR,
               "No RNDIS interface; enable USB tethering on the phone");
        at_usb_close(u);
        return NULL;
    }
    if (u->data_num == u->control_num) {
        /* Some gadgets put bulk endpoints on the same interface. */
        /* 일부 기기는 같은 인터페이스에 벌크 엔드포인트를 둔다. */
        u->data_num = (uint8_t)(u->control_num + 1);
    }

    if (open_one_interface(u->dev, u->control_num, &u->control_ifc) != 0) {
        at_log(AT_LOG_ERROR, "Failed to open RNDIS control interface %u",
               u->control_num);
        at_usb_close(u);
        return NULL;
    }

    /* Data interface: try requested number, then control+1, then any CDC Data. */
    /* 데이터 인터페이스: 요청 번호, control+1, 아무 CDC Data 순으로 시도. */
    if (open_one_interface(u->dev, u->data_num, &u->data_ifc) != 0) {
        at_log(AT_LOG_WARN, "Data interface %u open failed; rescanning",
               u->data_num);
        u->data_ifc = NULL;
    }

    if (u->data_ifc) {
        /* CDC Data often has endpoints only on alternate setting 1. */
        /* CDC Data는 대체 설정 1에만 엔드포인트가 있는 경우가 많다. */
        UInt8 n = 0;
        (*u->data_ifc)->GetNumEndpoints(u->data_ifc, &n);
        if (n == 0) {
            IOReturn ar = (*u->data_ifc)->SetAlternateInterface(u->data_ifc, 1);
            at_log(AT_LOG_INFO, "Data interface set alternate 1 => 0x%08x",
                   (unsigned)ar);
        }
        classify_pipes(u->data_ifc, &u->bulk_in, &u->bulk_out, &u->interrupt_in);
    }
    classify_pipes(u->control_ifc, &u->bulk_in, &u->bulk_out, &u->interrupt_in);

    if (u->bulk_in == 0 || u->bulk_out == 0) {
        at_log(AT_LOG_ERROR, "Missing bulk pipes (in=%u out=%u)",
               u->bulk_in, u->bulk_out);
        at_usb_close(u);
        return NULL;
    }

    u->opened = 1;

    /* USB bus speed is the hard ceiling for userspace RNDIS. */
    /* USB 버스 속도가 사용자 공간 RNDIS의 실제 상한이다. */
    if (u->dev) {
        UInt8 sp = 0;
        if ((*u->dev)->GetDeviceSpeed(u->dev, &sp) == kIOReturnSuccess) {
            /* 0=Low 1.5, 1=Full 12, 2=High 480, 3=Super 5000, 4+=SuperSpeedPlus */
            if (sp == 0) u->bus_mbps = 2;
            else if (sp == 1) u->bus_mbps = 12;
            else if (sp == 2) u->bus_mbps = 480;
            else if (sp == 3) u->bus_mbps = 5000;
            else u->bus_mbps = 10000;
        }
    }
    {
        at_ifc_if_t ifc = u->data_ifc ? u->data_ifc : u->control_ifc;
        UInt8 dir = 0, num = 0, xfer = 0, interval = 0;
        UInt16 mps_in = 0, mps_out = 0;
        (void)(*ifc)->GetPipeProperties(ifc, u->bulk_in, &dir, &num, &xfer, &mps_in, &interval);
        (void)(*ifc)->GetPipeProperties(ifc, u->bulk_out, &dir, &num, &xfer, &mps_out, &interval);
        at_log(AT_LOG_INFO,
               "USB RNDIS opened: %u Mbps  ctrl=%u data=%u IN=%u/%u OUT=%u/%u",
               u->bus_mbps, u->control_num, u->data_num,
               u->bulk_in, mps_in, u->bulk_out, mps_out);
    }
    return u;
}

void at_usb_close(at_usb_rndis_t *u) {
    if (!u) return;
    at_usb_async_detach(u);
    pthread_mutex_lock(&u->mu);
    if (u->data_ifc) {
        (*u->data_ifc)->USBInterfaceClose(u->data_ifc);
        (*u->data_ifc)->Release(u->data_ifc);
        u->data_ifc = NULL;
    }
    if (u->control_ifc) {
        (*u->control_ifc)->USBInterfaceClose(u->control_ifc);
        (*u->control_ifc)->Release(u->control_ifc);
        u->control_ifc = NULL;
    }
    if (u->dev) {
        (*u->dev)->USBDeviceClose(u->dev);
        (*u->dev)->Release(u->dev);
        u->dev = NULL;
    }
    if (u->device_service) {
        IOObjectRelease(u->device_service);
        u->device_service = 0;
    }
    pthread_mutex_unlock(&u->mu);
    pthread_mutex_destroy(&u->mu);
    free(u);
}

static uint32_t next_xid(at_usb_rndis_t *u) {
    u->xid++;
    if (u->xid == 0) u->xid = 1;
    return u->xid;
}

static int control_out(at_usb_rndis_t *u, void *data, uint16_t len) {
    if (!u || !u->control_ifc || atomic_load(&u->gone)) return -1;
    IOUSBDevRequestTO req;
    memset(&req, 0, sizeof(req));
    req.bmRequestType = USB_BM_OUT_CLASS_IFACE;
    req.bRequest = USB_CDC_SEND_ENCAPSULATED;
    req.wValue = 0;
    req.wIndex = u->control_num;
    req.wLength = len;
    req.pData = data;
    req.noDataTimeout = 1000;
    req.completionTimeout = 5000;
    IOReturn r = (*u->control_ifc)->ControlRequestTO(u->control_ifc, 0, &req);
    if (r != kIOReturnSuccess) {
        if (!atomic_load(&u->gone))
            at_log(AT_LOG_ERROR, "SendEncapsulatedCommand failed 0x%08x", (unsigned)r);
        return -1;
    }
    return 0;
}

static int control_in(at_usb_rndis_t *u, uint8_t *data, uint16_t cap, size_t *got) {
    IOUSBDevRequestTO req;
    memset(&req, 0, sizeof(req));
    req.bmRequestType = USB_BM_IN_CLASS_IFACE;
    req.bRequest = USB_CDC_GET_ENCAPSULATED;
    req.wValue = 0;
    req.wIndex = u->control_num;
    req.wLength = cap;
    req.pData = data;
    req.noDataTimeout = 1000;
    req.completionTimeout = 5000;
    IOReturn r = (*u->control_ifc)->ControlRequestTO(u->control_ifc, 0, &req);
    if (r != kIOReturnSuccess) return -1;
    if (got) *got = req.wLenDone;
    return 0;
}

static int rndis_rpc(at_usb_rndis_t *u, uint8_t *msg, size_t msg_len,
                     uint8_t *resp, size_t resp_cap, size_t *resp_len,
                     uint32_t expect_type, uint32_t expect_id) {
    (void)expect_type;
    if (control_out(u, msg, (uint16_t)msg_len) != 0) return -1;

    /* Some phones only complete after the interrupt endpoint is polled. */
    /* 일부 폰은 인터럽트 엔드포인트를 읽어야 제어 응답을 준다. */
    if (u->interrupt_in && u->control_ifc) {
        uint8_t note[16];
        UInt32 n = sizeof(note);
        (void)(*u->control_ifc)->ReadPipeTO(u->control_ifc, u->interrupt_in, note, &n, 50, 200);
    }

    for (int i = 0; i < 12; i++) {
        size_t got = 0;
        memset(resp, 0, resp_cap);
        if (control_in(u, resp, (uint16_t)MIN(resp_cap, 1024), &got) == 0 && got >= 8) {
            uint32_t type = at_rd32le(resp);
            uint32_t rid = (got >= 12) ? at_rd32le(resp + 8) : 0;
            if (type == RNDIS_MSG_KEEPALIVE) {
                uint8_t km[16];
                size_t klen = rndis_build_keepalive_cmplt(km, sizeof(km), rid);
                (void)control_out(u, km, (uint16_t)klen);
                continue;
            }
            if (type == RNDIS_MSG_INDICATE_STATUS) {
                uint32_t st = 0;
                rndis_parse_status(resp, got, &st);
                at_log(AT_LOG_INFO, "RNDIS indicate status 0x%08x", st);
                continue;
            }
            if (rid == expect_id || expect_id == 0) {
                if (resp_len) *resp_len = got;
                return 0;
            }
        }
        usleep(40000);
    }
    at_log(AT_LOG_ERROR, "RNDIS RPC timeout");
    return -1;
}

int at_usb_rndis_init(at_usb_rndis_t *u, uint8_t mac_out[AT_MAC_LEN],
                      rndis_init_result_t *init_out) {
    if (!u || !u->opened) return -1;
    uint8_t msg[RNDIS_CONTROL_BUF];
    uint8_t resp[RNDIS_CONTROL_BUF];
    size_t rlen = 0;
    rndis_init_result_t init;
    memset(&init, 0, sizeof(init));

    pthread_mutex_lock(&u->mu);
    uint32_t id = next_xid(u);
    size_t mlen = rndis_build_initialize(msg, sizeof(msg), id, AT_MAX_TRANSFER);
    int rc = rndis_rpc(u, msg, mlen, resp, sizeof(resp), &rlen, RNDIS_MSG_INITIALIZE_CMPLT, id);
    if (rc == 0) rc = rndis_parse_initialize_cmplt(resp, rlen, id, &init);
    if (rc != 0) {
        pthread_mutex_unlock(&u->mu);
        at_log(AT_LOG_ERROR, "RNDIS INITIALIZE failed (%d)", rc);
        return -1;
    }
    if (init.max_transfer_size >= 1024 && init.max_transfer_size <= AT_MAX_TRANSFER)
        u->max_xfer = init.max_transfer_size;
    u->max_packets = init.max_packets_per_xfer ? init.max_packets_per_xfer : 1;
    if (u->max_packets > 32) u->max_packets = 32;
    u->packet_align_shift = init.packet_alignment;
    if (u->packet_align_shift < 2) u->packet_align_shift = 2;
    if (u->packet_align_shift > 8) u->packet_align_shift = 8;
    if (init_out) *init_out = init;
    at_log(AT_LOG_INFO,
           "RNDIS initialized: max_xfer=%u max_pkts=%u align=%uB",
           u->max_xfer, u->max_packets, 1u << u->packet_align_shift);

    /* Query MAC: Linux sends 48 dummy bytes for ActiveSync compatibility. */
    /* MAC 조회: Linux는 ActiveSync 호환을 위해 더미 48바이트를 보낸다. */
    uint8_t dummy[48];
    memset(dummy, 0, sizeof(dummy));
    uint8_t mac[AT_MAC_LEN];
    memset(mac, 0, sizeof(mac));
    int got_mac = 0;
    uint32_t mac_oids[] = { RNDIS_OID_802_3_PERMANENT_ADDRESS, RNDIS_OID_802_3_CURRENT_ADDRESS };
    for (size_t i = 0; i < sizeof(mac_oids) / sizeof(mac_oids[0]) && !got_mac; i++) {
        id = next_xid(u);
        mlen = rndis_build_query(msg, sizeof(msg), id, mac_oids[i], dummy, 48);
        rlen = 0;
        if (rndis_rpc(u, msg, mlen, resp, sizeof(resp), &rlen, RNDIS_MSG_QUERY_CMPLT, id) != 0)
            continue;
        const uint8_t *info = NULL;
        uint32_t ilen = 0;
        if (rndis_parse_query_cmplt(resp, rlen, id, &info, &ilen) == 0 && ilen >= AT_MAC_LEN) {
            memcpy(mac, info, AT_MAC_LEN);
            got_mac = 1;
        }
    }
    if (!got_mac) {
        /* Locally administered fallback MAC / 로컬 관리 주소로 대체 */
        mac[0] = 0x0A;
        mac[1] = 0x54;
        mac[2] = 0x48;
        mac[3] = 0x00;
        mac[4] = 0x00;
        mac[5] = 0x01;
        at_log(AT_LOG_WARN, "Could not read device MAC; using fallback");
    }
    if (mac_out) memcpy(mac_out, mac, AT_MAC_LEN);
    char macs[24];
    at_mac_format(mac, macs, sizeof(macs));
    at_log(AT_LOG_INFO, "RNDIS MAC %s", macs);

    /* Optional capacity OIDs; failure is non-fatal. */
    /* 용량 OID는 선택이다. 실패해도 치명적이지 않다. */
    {
        uint32_t speed100 = 0, frame = 0;
        uint32_t qid = next_xid(u);
        uint8_t qmsg[RNDIS_CONTROL_BUF], qresp[RNDIS_CONTROL_BUF];
        size_t qlen = rndis_build_query(qmsg, sizeof(qmsg), qid, RNDIS_OID_GEN_LINK_SPEED, NULL, 0);
        size_t qrlen = 0;
        if (qlen && rndis_rpc(u, qmsg, qlen, qresp, sizeof(qresp), &qrlen,
                              RNDIS_MSG_QUERY_CMPLT, qid) == 0) {
            const uint8_t *info = NULL;
            uint32_t ilen = 0;
            if (rndis_parse_query_cmplt(qresp, qrlen, qid, &info, &ilen) == 0 &&
                info && ilen >= 4) {
                speed100 = at_rd32le(info);
                if (speed100 > 0) u->rndis_link_mbps = speed100 / 10000u;
            }
        }
        qid = next_xid(u);
        qlen = rndis_build_query(qmsg, sizeof(qmsg), qid, RNDIS_OID_GEN_MAXIMUM_FRAME_SIZE, NULL, 0);
        qrlen = 0;
        if (qlen && rndis_rpc(u, qmsg, qlen, qresp, sizeof(qresp), &qrlen,
                              RNDIS_MSG_QUERY_CMPLT, qid) == 0) {
            const uint8_t *info = NULL;
            uint32_t ilen = 0;
            if (rndis_parse_query_cmplt(qresp, qrlen, qid, &info, &ilen) == 0 &&
                info && ilen >= 4) {
                frame = at_rd32le(info);
                u->max_frame = frame;
            }
        }
        at_log(AT_LOG_INFO, "RNDIS capacity OIDs: link=%u Mbps frame=%u",
               u->rndis_link_mbps, u->max_frame);
    }

    /* Non-zero packet filter is what actually starts frame delivery. */
    /* 0이 아닌 패킷 필터를 설정해야 프레임 전달이 시작된다. */
    uint8_t filter[4];
    at_wr32le(filter, RNDIS_DEFAULT_FILTER);
    id = next_xid(u);
    mlen = rndis_build_set(msg, sizeof(msg), id, RNDIS_OID_GEN_CURRENT_PACKET_FILTER, filter, 4);
    rlen = 0;
    rc = rndis_rpc(u, msg, mlen, resp, sizeof(resp), &rlen, RNDIS_MSG_SET_CMPLT, id);
    if (rc == 0) rc = rndis_parse_set_cmplt(resp, rlen, id);
    pthread_mutex_unlock(&u->mu);
    if (rc != 0) {
        at_log(AT_LOG_ERROR, "Failed to set packet filter");
        return -1;
    }
    at_log(AT_LOG_INFO, "RNDIS data path enabled");
    return 0;
}

int at_usb_rndis_halt(at_usb_rndis_t *u) {
    if (!u || !u->opened) return -1;
    if (at_usb_is_gone(u)) return 0;
    uint8_t msg[16];
    pthread_mutex_lock(&u->mu);
    uint32_t id = next_xid(u);
    size_t mlen = rndis_build_halt(msg, sizeof(msg), id);
    (void)control_out(u, msg, (uint16_t)mlen);
    pthread_mutex_unlock(&u->mu);
    return 0;
}

int at_usb_bulk_write(at_usb_rndis_t *u, const uint8_t *data, size_t len) {
    if (!u || !data || len == 0) return -1;
    at_ifc_if_t ifc = u->data_ifc ? u->data_ifc : u->control_ifc;
    if (!ifc) return -1;
    IOReturn r = (*ifc)->WritePipeTO(ifc, u->bulk_out, (void *)data, (UInt32)len, 1000, 3000);
    if (r != kIOReturnSuccess) {
        if (r == kIOUSBPipeStalled)
            (void)(*ifc)->ClearPipeStallBothEnds(ifc, u->bulk_out);
        static int logged;
        if (!logged) {
            logged = 1;
            at_log(AT_LOG_WARN, "Bulk OUT failed 0x%08x", (unsigned)r);
        }
        return -1;
    }
    return 0;
}

int at_usb_bulk_read(at_usb_rndis_t *u, uint8_t *data, size_t cap, size_t *got,
                     uint32_t timeout_ms) {
    if (!u || !data || cap == 0) return -1;
    at_ifc_if_t ifc = u->data_ifc ? u->data_ifc : u->control_ifc;
    if (!ifc) {
        if (got) *got = 0;
        return -1;
    }
    UInt32 n = (UInt32)cap;
    /* IOKit treats noDataTimeout=0 as "wait forever"; never pass 0. */
    /* IOKit에서 noDataTimeout=0 은 무한 대기이므로 0을 넘기지 않는다. */
    if (timeout_ms == 0) timeout_ms = 1;
    IOReturn r = (*ifc)->ReadPipeTO(ifc, u->bulk_in, data, &n, timeout_ms, timeout_ms + 2000);
    /* Only Success/Underrun are real data. Timeout often leaves n==cap unchanged. */
    /* Success/Underrun 만 실제 데이터다. 타임아웃은 n 이 cap 그대로인 경우가 많다. */
    if ((r == kIOReturnSuccess || r == kIOReturnUnderrun) && n > 0) {
        if (got) *got = n;
        return 0;
    }
    if (r == kIOReturnTimeout || r == kIOReturnAborted || r == kIOUSBTransactionTimeout) {
        if (got) *got = 0;
        return 1;
    }
    if (got) *got = 0;
    if (r == kIOReturnNotOpen || r == kIOReturnNoDevice ||
        r == kIOReturnNotResponding || r == kIOReturnOffline) {
        atomic_store(&u->gone, 1);
    } else {
        static int logged;
        if (!logged) {
            logged = 1;
            at_log(AT_LOG_WARN, "Bulk IN failed 0x%08x n=%u",
                   (unsigned)r, (unsigned)n);
        }
    }
    return -1;
}

int at_usb_poll_interrupt(at_usb_rndis_t *u, uint32_t timeout_ms) {
    if (!u || !u->interrupt_in) return 0;
    pthread_mutex_lock(&u->mu);
    if (!u->control_ifc) {
        pthread_mutex_unlock(&u->mu);
        return 0;
    }
    uint8_t note[16];
    UInt32 n = sizeof(note);
    IOReturn r = (*u->control_ifc)->ReadPipeTO(u->control_ifc, u->interrupt_in, note, &n,
                                               timeout_ms, timeout_ms + 200);
    pthread_mutex_unlock(&u->mu);
    if (r != kIOReturnSuccess) return 0;
    return 1;
}

int at_usb_handle_control_events(at_usb_rndis_t *u) {
    if (!u) return -1;
    uint8_t resp[RNDIS_CONTROL_BUF];
    size_t got = 0;
    pthread_mutex_lock(&u->mu);
    int rc = control_in(u, resp, sizeof(resp), &got);
    if (rc == 0 && got >= 8) {
        uint32_t type = at_rd32le(resp);
        uint32_t rid = (got >= 12) ? at_rd32le(resp + 8) : 0;
        if (type == RNDIS_MSG_KEEPALIVE) {
            uint8_t km[16];
            size_t klen = rndis_build_keepalive_cmplt(km, sizeof(km), rid);
            (void)control_out(u, km, (uint16_t)klen);
        }
    }
    pthread_mutex_unlock(&u->mu);
    return 0;
}

int at_usb_keepalive(at_usb_rndis_t *u) {
    if (!u) return -1;
    uint8_t msg[16], resp[RNDIS_CONTROL_BUF];
    size_t rlen = 0;
    pthread_mutex_lock(&u->mu);
    uint32_t id = next_xid(u);
    size_t mlen = rndis_build_keepalive(msg, sizeof(msg), id);
    int rc = rndis_rpc(u, msg, mlen, resp, sizeof(resp), &rlen, RNDIS_MSG_KEEPALIVE_CMPLT, id);
    pthread_mutex_unlock(&u->mu);
    return rc;
}

uint32_t at_usb_max_transfer(const at_usb_rndis_t *u) {
    return u ? u->max_xfer : AT_MAX_TRANSFER;
}

uint32_t at_usb_max_packets(const at_usb_rndis_t *u) {
    return u && u->max_packets ? u->max_packets : 1;
}

uint32_t at_usb_packet_align(const at_usb_rndis_t *u) {
    return u && u->packet_align_shift ? u->packet_align_shift : 2;
}

uint32_t at_usb_bus_mbps(const at_usb_rndis_t *u) {
    return u && u->bus_mbps ? u->bus_mbps : 480;
}

uint32_t at_usb_rndis_link_mbps(const at_usb_rndis_t *u) {
    return u ? u->rndis_link_mbps : 0;
}

int at_usb_abort_in(at_usb_rndis_t *u) {
    if (!u) return -1;
    at_ifc_if_t ifc = u->data_ifc ? u->data_ifc : u->control_ifc;
    if (!ifc || u->bulk_in == 0) return -1;
    IOReturn r = (*ifc)->AbortPipe(ifc, u->bulk_in);
    return (r == kIOReturnSuccess || r == kIOReturnAborted) ? 0 : -1;
}

int at_usb_abort_out(at_usb_rndis_t *u) {
    if (!u) return -1;
    at_ifc_if_t ifc = u->data_ifc ? u->data_ifc : u->control_ifc;
    if (!ifc || u->bulk_out == 0) return -1;
    IOReturn r = (*ifc)->AbortPipe(ifc, u->bulk_out);
    return (r == kIOReturnSuccess || r == kIOReturnAborted) ? 0 : -1;
}

static at_ifc_if_t data_ifc(at_usb_rndis_t *u) {
    return u->data_ifc ? u->data_ifc : u->control_ifc;
}

static void async_in_done(void *refcon, IOReturn result, void *arg0) {
    /* refcon is &u->in_urb[i]; the first field of that struct is u. */
    /* refcon 은 &u->in_urb[i] 이며, 그 구조체의 첫 필드는 u 이다. */
    at_usb_rndis_t *u = *(at_usb_rndis_t **)refcon;
    if (!u) return;
    uint8_t *buf = NULL;
    for (int i = 0; i < AT_USB_IN_DEPTH; i++) {
        if ((void *)&u->in_urb[i] == refcon) {
            buf = u->in_urb[i].buf;
            atomic_store(&u->in_urb[i].busy, 0);
            break;
        }
    }
    size_t n = (size_t)(uintptr_t)arg0;
    int rc = 0;
    if (result == kIOReturnTimeout || result == kIOUSBTransactionTimeout ||
        result == kIOReturnAborted) {
        rc = 1;
        n = 0;
    } else if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        rc = -1;
        n = 0;
        if (result == kIOReturnNotOpen || result == kIOReturnNoDevice ||
            result == kIOReturnNotResponding || result == kIOReturnOffline)
            atomic_store(&u->gone, 1);
    }
    if (u->in_cb) u->in_cb(u->in_user, rc, n, buf);
}

static void async_out_done(void *refcon, IOReturn result, void *arg0) {
    at_usb_rndis_t *u = refcon;
    (void)arg0;
    atomic_store(&u->out_urb.busy, 0);
    int rc = 0;
    if (result == kIOReturnTimeout || result == kIOUSBTransactionTimeout ||
        result == kIOReturnAborted) {
        rc = 1;
    } else if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        rc = -1;
    }
    if (u->out_cb) u->out_cb(u->out_user, rc);
}

static void usb_interest(void *refcon, io_service_t service,
                         uint32_t messageType, void *messageArgument) {
    (void)service;
    (void)messageArgument;
    at_usb_rndis_t *u = refcon;
    if (!u) return;
    if (messageType == kIOMessageServiceIsTerminated ||
        messageType == kIOMessageServiceIsRequestingClose ||
        messageType == kIOMessageServiceWasClosed) {
        atomic_store(&u->gone, 1);
        if (u->async_rl) CFRunLoopWakeUp(u->async_rl);
    }
}

static void detach_unplug_watch(at_usb_rndis_t *u) {
    if (!u) return;
    if (u->interest) {
        IOObjectRelease(u->interest);
        u->interest = 0;
    }
    if (u->notify_port) {
        if (u->async_rl) {
            CFRunLoopSourceRef src = IONotificationPortGetRunLoopSource(u->notify_port);
            if (src) CFRunLoopRemoveSource(u->async_rl, src, kCFRunLoopDefaultMode);
        }
        IONotificationPortDestroy(u->notify_port);
        u->notify_port = NULL;
    }
}

static void attach_unplug_watch(at_usb_rndis_t *u) {
    if (!u || !u->device_service || u->notify_port || !u->async_rl) return;
    u->notify_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!u->notify_port) return;
    CFRunLoopSourceRef src = IONotificationPortGetRunLoopSource(u->notify_port);
    if (src)
        CFRunLoopAddSource(u->async_rl, src, kCFRunLoopDefaultMode);
    kern_return_t kr = IOServiceAddInterestNotification(
        u->notify_port, u->device_service, kIOGeneralInterest,
        usb_interest, u, &u->interest);
    if (kr != KERN_SUCCESS) {
        at_log(AT_LOG_WARN, "USB unplug watch failed 0x%x", (unsigned)kr);
        detach_unplug_watch(u);
    }
}

int at_usb_is_gone(const at_usb_rndis_t *u) {
    return !u || atomic_load(&u->gone);
}

int at_usb_async_attach(at_usb_rndis_t *u) {
    if (!u) return -1;
    at_ifc_if_t ifc = data_ifc(u);
    if (!ifc) return -1;
    CFRunLoopSourceRef src = NULL;
    IOReturn r = (*ifc)->CreateInterfaceAsyncEventSource(ifc, &src);
    if (r != kIOReturnSuccess || !src) {
        at_log(AT_LOG_WARN, "USB async event source failed 0x%08x",
               (unsigned)r);
        return -1;
    }
    u->async_src = src;
    u->async_rl = CFRunLoopGetCurrent();
    CFRunLoopAddSource(u->async_rl, u->async_src, kCFRunLoopDefaultMode);
    u->in_cb = NULL;
    u->in_user = NULL;
    u->out_cb = NULL;
    u->out_user = NULL;
    for (int i = 0; i < AT_USB_IN_DEPTH; i++) {
        u->in_urb[i].u = u;
        u->in_urb[i].buf = NULL;
        atomic_init(&u->in_urb[i].busy, 0);
    }
    u->out_urb.u = u;
    u->out_urb.buf = NULL;
    atomic_init(&u->out_urb.busy, 0);
    attach_unplug_watch(u);
    return 0;
}

void at_usb_async_detach(at_usb_rndis_t *u) {
    if (!u) return;
    detach_unplug_watch(u);
    if (u->async_src && u->async_rl) {
        CFRunLoopRemoveSource(u->async_rl, u->async_src, kCFRunLoopDefaultMode);
        CFRelease(u->async_src);
    }
    u->async_src = NULL;
    u->async_rl = NULL;
    u->in_cb = NULL;
    u->in_user = NULL;
    u->out_cb = NULL;
    u->out_user = NULL;
}

int at_usb_async_submit_in(at_usb_rndis_t *u, uint8_t *buf, size_t cap,
                           at_usb_in_cb cb, void *user) {
    if (!u || !buf || cap == 0) return -1;
    int slot = -1;
    for (int i = 0; i < AT_USB_IN_DEPTH; i++) {
        if (!atomic_exchange(&u->in_urb[i].busy, 1)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0;
    u->in_cb = cb;
    u->in_user = user;
    u->in_urb[slot].u = u;
    u->in_urb[slot].buf = buf;
    at_ifc_if_t ifc = data_ifc(u);
    /* ReadPipeAsync has no timeout, so the IN URB stays queued until data or AbortPipe.
     * Never call sync WritePipe from this completion — IOKit serializes and caps TCP. */
    /* ReadPipeAsync 는 타임아웃이 없어 데이터나 AbortPipe 때까지 IN URB가 남는다.
     * 이 완료 콜백에서 동기 WritePipe 를 호출하면 IOKit가 직렬화해 TCP가 묶인다. */
    IOReturn r = (*ifc)->ReadPipeAsync(ifc, u->bulk_in, buf, (UInt32)cap,
                                       async_in_done, &u->in_urb[slot]);
    if (r != kIOReturnSuccess) {
        r = (*ifc)->ReadPipeAsyncTO(ifc, u->bulk_in, buf, (UInt32)cap,
                                    60000, 120000, async_in_done, &u->in_urb[slot]);
    }
    if (r != kIOReturnSuccess) {
        atomic_store(&u->in_urb[slot].busy, 0);
        if (r == kIOReturnNotOpen || r == kIOReturnNoDevice ||
            r == kIOReturnNotResponding || r == kIOReturnOffline) {
            atomic_store(&u->gone, 1);
        } else if (!atomic_load(&u->gone)) {
            at_log(AT_LOG_WARN, "ReadPipeAsync failed 0x%08x", (unsigned)r);
        }
        return -1;
    }
    return 0;
}

int at_usb_async_submit_out(at_usb_rndis_t *u, uint8_t *buf, size_t len,
                            at_usb_out_cb cb, void *user) {
    if (!u || !buf || len == 0) return -1;
    if (atomic_exchange(&u->out_urb.busy, 1)) return 1;
    u->out_cb = cb;
    u->out_user = user;
    u->out_urb.u = u;
    u->out_urb.buf = buf;
    at_ifc_if_t ifc = data_ifc(u);
    IOReturn r = (*ifc)->WritePipeAsync(ifc, u->bulk_out, buf, (UInt32)len,
                                        async_out_done, u);
    if (r != kIOReturnSuccess) {
        r = (*ifc)->WritePipeAsyncTO(ifc, u->bulk_out, buf, (UInt32)len,
                                     1000, 3000, async_out_done, u);
    }
    if (r != kIOReturnSuccess) {
        atomic_store(&u->out_urb.busy, 0);
        static int logged;
        if (!logged) {
            logged = 1;
            at_log(AT_LOG_WARN, "WritePipeAsync failed 0x%08x",
                   (unsigned)r);
        }
        return -1;
    }
    return 0;
}

int at_usb_async_out_busy(const at_usb_rndis_t *u) {
    return u ? atomic_load(&u->out_urb.busy) : 0;
}

int at_usb_async_run(at_usb_rndis_t *u, double seconds) {
    if (!u || !u->async_rl) return -1;
    (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
    return 0;
}

void at_usb_async_wake(at_usb_rndis_t *u) {
    if (u && u->async_rl) CFRunLoopWakeUp(u->async_rl);
}

int at_usb_clear_stall(at_usb_rndis_t *u, int is_in) {
    if (!u) return -1;
    at_ifc_if_t ifc = u->data_ifc ? u->data_ifc : u->control_ifc;
    if (!ifc) return -1;
    UInt8 pipe = is_in ? u->bulk_in : u->bulk_out;
    if (pipe == 0) return -1;
    IOReturn r = (*ifc)->ClearPipeStallBothEnds(ifc, pipe);
    if (r != kIOReturnSuccess) r = (*ifc)->ClearPipeStall(ifc, pipe);
    return r == kIOReturnSuccess ? 0 : -1;
}
