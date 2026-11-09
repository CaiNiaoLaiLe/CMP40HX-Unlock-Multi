#include "uefi_min.h"
#include "core_abi.h"
#include "elf_loader.h"
#include "booter.h"

#define CMP_VENDOR_ID 0x10deU
#define CMP90HX_DEVICE_ID 0x220dU
#define CMP40HX_DEVICE_ID 0x1f0bU
#define NV_PMC_BOOT_0 0x00000000U
#define BOOT0_ARCH_MASK (0x1fU << 24)
#define BOOT0_ARCH_GA10X (0x17U << 24)
#define BOOT0_IMPL_MASK (0x0fU << 20)
#define BOOT0_IMPL_GA102 (0x02U << 20)
#define BOOT0_ARCH_TU10X (0x1aU << 24)   /* (upstream label) */
#define BOOT0_ARCH_TU106 (0x16U << 24)   /* Turing family: TU106/CMP40HX BOOT0=0x16xxxxxx */
#define PCI_COMMAND_OFFSET 0x04U
#define PCI_COMMAND_MEMORY 0x0002U
#define PCI_COMMAND_MASTER 0x0004U
#define PCI_BAR0_OFFSET 0x10U
#define PCI_BRIDGE_BUS_OFFSET 0x18U
#define PCI_BRIDGE_CONTROL_OFFSET 0x3eU
#define PCI_BRIDGE_CTL_BUS_RESET 0x0040U
#define PCI_CLASS_BRIDGE_PCI 0x0604U
/* PCIe extended config-space capability layout, used for the Gen2 retrain. */
#define PCI_CAPABILITY_LIST 0x34U
#define PCI_CAP_ID_EXP 0x10U
#define PCI_EXP_LNKCAP 0x0cU
#define PCI_EXP_LNKCAP2 0x2cU
#define PCI_EXP_LNKCTL 0x10U
#define PCI_EXP_LNKSTA 0x12U
#define PCI_EXP_LNKCTL2 0x30U
#define PCI_EXP_LNKCTL_ASPM 0x0003U
#define PCI_EXP_LNKCTL_RETRAIN 0x0020U
#define PCI_EXP_LNKCTL_CCC 0x0040U
#define PCI_EXP_LNKCTL_HAWD 0x0200U
#define PCI_EXP_LNKCTL2_HASD 0x0020U
#define PCI_EXP_LNKCTL2_TLS_MASK 0x000fU
#define PCI_EXP_LNKSTA_LBMS 0x4000U
#define PCI_EXP_LNKSTA_SPEED_MASK 0x000fU
/* BAR0 XVE/HUBMMU-domain policy registers for the verified Gen2 sequence. */
#define GEN2_OVR_OFF  0x0008872CU
#define GEN2_CFG_OFF  0x0008c040U
#define GEN2_PL_OFF   0x0008c1c0U
#define GEN2_MISC_OFF 0x0008841cU
#define GEN2_CYA_OFF  0x0008c2c0U
#define FB_WINDOW_SIZE 0x10000000ULL
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PAGE_SIZE 4096ULL

extern const UINT8 _binary_nvpermissive_core_o_start[];
extern const UINT8 _binary_nvpermissive_core_o_end[];

static EFI_SYSTEM_TABLE *g_system_table;
static EFI_BOOT_SERVICES *g_boot_services;

/* Booter hard-hang diagnosis: every console_write line is also flushed to an
 * ESP file so a blind (remote, no-screen) reboot can be read from Linux via
 * /boot/efi/EFI/UNLOCK/LOGS/BOOTER_TRACE.TXT. Overwritten clean each boot.
 * Best-effort: if the file cannot be opened the unlock still runs, only the
 * trace is lost. */
static EFI_FILE *g_trace_file;
static EFI_FILE *g_trace_root;

struct pci_location {
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *rb;
    UINT16 segment;
    UINT8 bus;
    UINT8 device;
    UINT8 function;
};

struct platform_gpu {
    struct pci_location gpu;
    struct pci_location bridge;
    UINT32 expected_id;
    UINT64 bar0;
    UINT64 bar1;
    UINT32 saved_bars[6];
    UINT16 saved_command;
};

enum requested_mode {
    REQUEST_EXIT,
    REQUEST_INSPECT,
    REQUEST_BOOTER,
    REQUEST_UNKNOWN
};

static void trace_tee(const char *text)
{
    UINTN len = 0;
    if (!g_trace_file || !text)
        return;
    while (text[len])
        len++;
    if (len) {
        g_trace_file->Write(g_trace_file, &len, text);
        g_trace_file->Flush(g_trace_file);   /* survive a later hard hang */
    }
}

static void console_write(const char *text)
{
    CHAR16 wide[256];
    UINTN used = 0;

    trace_tee(text);
    if (!g_system_table || !g_system_table->ConOut)
        return;
    while (*text) {
        if (*text == '\n') {
            if (used + 2 >= ARRAY_SIZE(wide)) {
                wide[used] = 0;
                g_system_table->ConOut->OutputString(g_system_table->ConOut, wide);
                used = 0;
            }
            wide[used++] = '\r';
        }
        wide[used++] = (UINT8)*text++;
        if (used + 1 >= ARRAY_SIZE(wide)) {
            wide[used] = 0;
            g_system_table->ConOut->OutputString(g_system_table->ConOut, wide);
            used = 0;
        }
    }
    if (used) {
        wide[used] = 0;
        g_system_table->ConOut->OutputString(g_system_table->ConOut, wide);
    }
}

static UINTN append_char(char *out, UINTN cap, UINTN pos, char value)
{
    if (pos + 1 < cap)
        out[pos] = value;
    return pos + 1;
}

static UINTN append_string(char *out, UINTN cap, UINTN pos, const char *text)
{
    if (!text)
        text = "(null)";
    while (*text)
        pos = append_char(out, cap, pos, *text++);
    return pos;
}

static UINTN append_number(char *out, UINTN cap, UINTN pos, UINT64 value,
                           UINT32 base, int upper, UINTN width, char pad,
                           int negative)
{
    char digits[32];
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    UINTN count = 0;
    UINTN total;

    do {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while (value && count < ARRAY_SIZE(digits));
    total = count + (negative ? 1U : 0U);
    if (negative && pad == '0')
        pos = append_char(out, cap, pos, '-');
    while (total < width) {
        pos = append_char(out, cap, pos, pad);
        total++;
    }
    if (negative && pad != '0')
        pos = append_char(out, cap, pos, '-');
    while (count)
        pos = append_char(out, cap, pos, digits[--count]);
    return pos;
}

static UINTN format_message(char *out, UINTN cap, const char *format,
                            __builtin_va_list args)
{
    UINTN pos = 0;

    while (*format) {
        char pad = ' ';
        UINTN width = 0;
        int length = 0;
        char spec;

        if (*format != '%') {
            pos = append_char(out, cap, pos, *format++);
            continue;
        }
        format++;
        if (*format == '%') {
            pos = append_char(out, cap, pos, *format++);
            continue;
        }
        while (*format == '#' || *format == '-' || *format == '+' || *format == ' ')
            format++;
        if (*format == '0') {
            pad = '0';
            format++;
        }
        while (*format >= '0' && *format <= '9')
            width = width * 10 + (UINTN)(*format++ - '0');
        if (*format == '.') {
            format++;
            while (*format >= '0' && *format <= '9')
                format++;
        }
        if (*format == 'l') {
            length = 1;
            format++;
            if (*format == 'l') {
                length = 2;
                format++;
            }
        } else if (*format == 'z') {
            length = 3;
            format++;
        }
        spec = *format ? *format++ : 0;
        if (spec == 's') {
            pos = append_string(out, cap, pos, __builtin_va_arg(args, const char *));
        } else if (spec == 'c') {
            pos = append_char(out, cap, pos,
                              (char)__builtin_va_arg(args, int));
        } else if (spec == 'p') {
            UINT64 value = (UINT64)(uintptr_t)__builtin_va_arg(args, void *);
            pos = append_string(out, cap, pos, "0x");
            pos = append_number(out, cap, pos, value, 16, 0,
                                width ? width : 1, '0', 0);
        } else if (spec == 'x' || spec == 'X' || spec == 'u') {
            UINT64 value;
            if (length == 2)
                value = __builtin_va_arg(args, unsigned long long);
            else if (length == 1)
                value = __builtin_va_arg(args, unsigned long);
            else if (length == 3)
                value = __builtin_va_arg(args, size_t);
            else
                value = __builtin_va_arg(args, unsigned int);
            pos = append_number(out, cap, pos, value,
                                spec == 'u' ? 10U : 16U,
                                spec == 'X', width, pad, 0);
        } else if (spec == 'd' || spec == 'i') {
            INT64 signed_value;
            UINT64 value;
            int negative;
            if (length == 2)
                signed_value = __builtin_va_arg(args, long long);
            else if (length == 1)
                signed_value = __builtin_va_arg(args, long);
            else if (length == 3)
                signed_value = (INT64)__builtin_va_arg(args, ptrdiff_t);
            else
                signed_value = __builtin_va_arg(args, int);
            negative = signed_value < 0;
            value = negative ? (UINT64)(-(signed_value + 1)) + 1U
                             : (UINT64)signed_value;
            pos = append_number(out, cap, pos, value, 10, 0, width, pad,
                                negative);
        } else {
            pos = append_char(out, cap, pos, '%');
            if (spec)
                pos = append_char(out, cap, pos, spec);
        }
    }
    if (cap)
        out[pos < cap ? pos : cap - 1] = 0;
    return pos;
}

static void console_printf(const char *format, ...)
{
    char buffer[1024];
    __builtin_va_list args;
    __builtin_va_start(args, format);
    format_message(buffer, sizeof(buffer), format, args);
    __builtin_va_end(args);
    console_write(buffer);
}

void SYSVABI gpu_log(struct gpu_dev *g, const char *format, ...)
{
    char buffer[1024];
    __builtin_va_list args;
    (void)g;
    __builtin_va_start(args, format);
    format_message(buffer, sizeof(buffer), format, args);
    __builtin_va_end(args);
    console_write("[core] ");
    console_write(buffer);
}

static void to_wchar(const char *s, CHAR16 *out, UINTN cap)
{
    UINTN i = 0;
    for (; s[i] && i + 1 < cap; i++)
        out[i] = (CHAR16)s[i];
    out[i] = 0;
}

static EFI_STATUS ensure_dir(EFI_FILE *root, const char *path_ascii)
{
    CHAR16 w[72];
    EFI_FILE *dir = 0;
    EFI_STATUS st;

    if (!root || !root->Open)
        return EFI_INVALID_PARAMETER;
    to_wchar(path_ascii, w, ARRAY_SIZE(w));
    /* Open first: parents like \EFI\UNLOCK already exist, so only create
     * when truly missing. EFI_FILE_DIRECTORY must be 0x10 (bit 4). */
    st = root->Open(root, &dir, w, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) {
        dir = 0;
        st = root->Open(root, &dir, w,
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                EFI_FILE_DIRECTORY);
    }
    if (!EFI_ERROR(st) && dir)
        dir->Close(dir);
    return st;
}

static int trace_try_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs)
{
    EFI_FILE *root = 0, *old = 0, *file = 0;
    CHAR16 fp[72];

    if (!fs || !fs->OpenVolume || EFI_ERROR(fs->OpenVolume(fs, &root)) || !root)
        return 0;
    ensure_dir(root, "\\EFI\\UNLOCK");
    ensure_dir(root, "\\EFI\\UNLOCK\\LOGS");

    to_wchar("\\EFI\\UNLOCK\\LOGS\\BOOTER_TRACE.TXT", fp, ARRAY_SIZE(fp));
    /* clean overwrite: drop any previous file so no stale tail survives */
    if (!EFI_ERROR(root->Open(root, &old, fp,
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && old)
        old->Delete(old);   /* Delete closes the handle too */
    if (EFI_ERROR(root->Open(root, &file, fp,
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
            EFI_FILE_ARCHIVE)) || !file) {
        root->Close(root);
        return 0;
    }
    g_trace_root = root;   /* keep volume open for the file's lifetime */
    g_trace_file = file;
    return 1;
}

static void trace_open(EFI_HANDLE image_handle)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    EFI_HANDLE *handles = 0;
    UINTN count = 0, i;

    if (!g_boot_services)
        return;
    if (!EFI_ERROR(g_boot_services->HandleProtocol(
            image_handle, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID,
            (void **)&li)) && li) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        if (li->ParentHandle) {
            fs = 0;
            if (!EFI_ERROR(g_boot_services->HandleProtocol(
                    li->ParentHandle,
                    (EFI_GUID *)&EFI_FILE_SYSTEM_PROTOCOL_GUID, (void **)&fs))
                && trace_try_volume(fs))
                return;
        }
        if (li->DeviceHandle) {
            fs = 0;
            if (!EFI_ERROR(g_boot_services->HandleProtocol(
                    li->DeviceHandle,
                    (EFI_GUID *)&EFI_FILE_SYSTEM_PROTOCOL_GUID, (void **)&fs))
                && trace_try_volume(fs))
                return;
        }
    }
    /* fallback: every handle that publishes the file system protocol */
    if (!EFI_ERROR(g_boot_services->LocateHandleBuffer(
            ByProtocol, (EFI_GUID *)&EFI_FILE_SYSTEM_PROTOCOL_GUID, 0,
            &count, &handles))) {
        for (i = 0; i < count; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
            if (!EFI_ERROR(g_boot_services->HandleProtocol(
                    handles[i], (EFI_GUID *)&EFI_FILE_SYSTEM_PROTOCOL_GUID,
                    (void **)&fs)) && fs && trace_try_volume(fs))
                break;
        }
        g_boot_services->FreePool(handles);
    }
}

static void trace_close(void)
{
    if (g_trace_file) {
        g_trace_file->Flush(g_trace_file);
        g_trace_file->Close(g_trace_file);
        g_trace_file = 0;
    }
    if (g_trace_root) {
        g_trace_root->Close(g_trace_root);
        g_trace_root = 0;
    }
}

void *SYSVABI memcpy(void *destination, const void *source, size_t size)
{
    UINT8 *dst = (UINT8 *)destination;
    const UINT8 *src = (const UINT8 *)source;
    while (size--)
        *dst++ = *src++;
    return destination;
}

void *SYSVABI memset(void *destination, int value, size_t size)
{
    UINT8 *dst = (UINT8 *)destination;
    while (size--)
        *dst++ = (UINT8)value;
    return destination;
}

void SYSVABI memcpy_toio(volatile void *destination, const void *source,
                         size_t size)
{
    volatile UINT8 *dst = (volatile UINT8 *)destination;
    const UINT8 *src = (const UINT8 *)source;
    while (size--)
        *dst++ = *src++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

uint32_t SYSVABI ioread32(const volatile void *address)
{
    UINT32 value = *(const volatile UINT32 *)address;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return value;
}

void SYSVABI iowrite32(uint32_t value, volatile void *address)
{
    *(volatile UINT32 *)address = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static EFI_STATUS pci_read(const struct pci_location *location, UINT32 offset,
                           EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,
                           void *value)
{
    return location->rb->Pci.Read(location->rb, width,
        EFI_PCI_ADDRESS(location->bus, location->device,
                        location->function, offset), 1, value);
}

static EFI_STATUS pci_write(const struct pci_location *location, UINT32 offset,
                            EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,
                            void *value)
{
    return location->rb->Pci.Write(location->rb, width,
        EFI_PCI_ADDRESS(location->bus, location->device,
                        location->function, offset), 1, value);
}

static int find_cmp90hx(struct platform_gpu *platform)
{
    EFI_HANDLE *handles = 0;
    UINTN handle_count = 0;
    EFI_STATUS status;
    UINTN h;
    int matches = 0;

    status = g_boot_services->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)&EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID,
        0, &handle_count, &handles);
    if (EFI_ERROR(status))
        return -1;

    for (h = 0; h < handle_count; h++) {
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *rb = 0;
        UINT32 bus;
        status = g_boot_services->HandleProtocol(
            handles[h], (EFI_GUID *)&EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID,
            (void **)&rb);
        if (EFI_ERROR(status) || !rb)
            continue;
        for (bus = 0; bus < 256; bus++) {
            UINT32 dev;
            for (dev = 0; dev < 32; dev++) {
                UINT32 func;
                for (func = 0; func < 8; func++) {
                    struct pci_location location = {
                        rb, (UINT16)rb->SegmentNumber,
                        (UINT8)bus, (UINT8)dev, (UINT8)func
                    };
                    UINT32 id = 0xffffffffU;
                    status = pci_read(&location, 0, EfiPciWidthUint32, &id);
                    if (EFI_ERROR(status) || (id & 0xffffU) == 0xffffU)
                        continue;
                    if ((id & 0xffffU) == CMP_VENDOR_ID &&
                        ((id >> 16) == CMP90HX_DEVICE_ID ||
                         (id >> 16) == CMP40HX_DEVICE_ID)) {
                        if (matches == 0) {
                            matches = 1;
                            platform->gpu = location;
                            platform->expected_id = id;
                        } else if (platform->gpu.segment != location.segment ||
                                   platform->gpu.bus != location.bus ||
                                   platform->gpu.device != location.device ||
                                   platform->gpu.function != location.function) {
                            matches = 2;
                        }
                    }
                }
            }
        }
    }
    g_boot_services->FreePool(handles);
    return matches;
}

static int find_upstream_bridge(struct platform_gpu *platform)
{
    UINT32 bus;
    UINT32 best_span = 0xffffffffU;
    UINT32 best_secondary = 0;
    int found = 0;

    for (bus = 0; bus < 256; bus++) {
        UINT32 dev;
        for (dev = 0; dev < 32; dev++) {
            UINT32 func;
            for (func = 0; func < 8; func++) {
                struct pci_location location = {
                    platform->gpu.rb, platform->gpu.segment,
                    (UINT8)bus, (UINT8)dev, (UINT8)func
                };
                UINT32 id = 0xffffffffU;
                UINT32 class_revision = 0;
                UINT32 buses = 0;
                UINT32 secondary;
                UINT32 subordinate;
                UINT32 span;
                if (EFI_ERROR(pci_read(&location, 0, EfiPciWidthUint32, &id)) ||
                    (id & 0xffffU) == 0xffffU)
                    continue;
                if (EFI_ERROR(pci_read(&location, 0x08, EfiPciWidthUint32,
                                       &class_revision)) ||
                    ((class_revision >> 16) & 0xffffU) != PCI_CLASS_BRIDGE_PCI)
                    continue;
                if (EFI_ERROR(pci_read(&location, PCI_BRIDGE_BUS_OFFSET,
                                       EfiPciWidthUint32, &buses)))
                    continue;
                secondary = (buses >> 8) & 0xffU;
                subordinate = (buses >> 16) & 0xffU;
                if (secondary > platform->gpu.bus ||
                    subordinate < platform->gpu.bus)
                    continue;
                span = subordinate - secondary;
                if (!found || span < best_span ||
                    (span == best_span && secondary > best_secondary)) {
                    platform->bridge = location;
                    best_span = span;
                    best_secondary = secondary;
                    found = 1;
                }
            }
        }
    }
    return found ? 0 : -1;
}

static int decode_memory_bar(struct platform_gpu *platform, UINT32 offset,
                             UINT64 *address, UINT32 *consumed)
{
    UINT32 low;
    UINT32 high = 0;
    if (EFI_ERROR(pci_read(&platform->gpu, offset,
                           EfiPciWidthUint32, &low)) || (low & 1U))
        return -1;
    *consumed = 1;
    if (((low >> 1) & 3U) == 2U) {
        if (EFI_ERROR(pci_read(&platform->gpu, offset + 4,
                               EfiPciWidthUint32, &high)))
            return -1;
        *consumed = 2;
    }
    *address = ((UINT64)high << 32) | (low & ~0x0fU);
    return *address ? 0 : -1;
}

static int prepare_pci_resources(struct platform_gpu *platform)
{
    UINT32 bar0_slots;
    UINT32 bar1_slots;
    UINT32 bar1_offset;
    UINTN i;

    if (EFI_ERROR(pci_read(&platform->gpu, PCI_COMMAND_OFFSET,
                           EfiPciWidthUint16, &platform->saved_command)))
        return -1;
    for (i = 0; i < ARRAY_SIZE(platform->saved_bars); i++) {
        if (EFI_ERROR(pci_read(&platform->gpu,
                               PCI_BAR0_OFFSET + (UINT32)i * 4,
                               EfiPciWidthUint32,
                               &platform->saved_bars[i])))
            return -1;
    }
    if (decode_memory_bar(platform, PCI_BAR0_OFFSET,
                          &platform->bar0, &bar0_slots))
        return -1;
    bar1_offset = PCI_BAR0_OFFSET + bar0_slots * 4;
    if (decode_memory_bar(platform, bar1_offset,
                          &platform->bar1, &bar1_slots))
        return -1;
    (void)bar1_slots;
    platform->saved_command |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    if (EFI_ERROR(pci_write(&platform->gpu, PCI_COMMAND_OFFSET,
                            EfiPciWidthUint16,
                            &platform->saved_command)))
        return -1;
    return 0;
}

static int restore_gpu_config(struct platform_gpu *platform)
{
    UINTN i;
    for (i = 0; i < ARRAY_SIZE(platform->saved_bars); i++) {
        UINT32 value = platform->saved_bars[i];
        if (EFI_ERROR(pci_write(&platform->gpu,
                                PCI_BAR0_OFFSET + (UINT32)i * 4,
                                EfiPciWidthUint32, &value)))
            return -1;
    }
    if (EFI_ERROR(pci_write(&platform->gpu, PCI_COMMAND_OFFSET,
                            EfiPciWidthUint16,
                            &platform->saved_command)))
        return -1;
    return 0;
}

static void *SYSVABI host_alloc(struct gpu_dev *g, size_t size)
{
    void *memory = 0;
    struct platform_gpu *platform = (struct platform_gpu *)g->platform_data;
    (void)platform;
    if (EFI_ERROR(g_boot_services->AllocatePool(EfiLoaderData, size, &memory)))
        return 0;
    memset(memory, 0, size);
    return memory;
}

static void SYSVABI host_free(struct gpu_dev *g, void *memory)
{
    (void)g;
    if (memory)
        g_boot_services->FreePool(memory);
}

static void *SYSVABI dma_alloc_below_4g(struct gpu_dev *g, size_t size,
                                        uint64_t *physical)
{
    EFI_PHYSICAL_ADDRESS address = 0xffffffffULL;
    UINTN pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    (void)g;
    if (!physical || !pages ||
        EFI_ERROR(g_boot_services->AllocatePages(
            AllocateMaxAddress, EfiLoaderData, pages, &address)))
        return 0;
    memset((void *)(uintptr_t)address, 0, pages * PAGE_SIZE);
    *physical = address;
    return (void *)(uintptr_t)address;
}

/* Above-4GiB identity-mapped reserved allocator, a faithful port of the
 * v70 reference helper alloc_fwsec_buffer (unlock40x_v70.c L1245). The TU102
 * booter reads WPR meta / V67 out of sysmem through a 64-bit mailbox and the
 * locked 40HX blocks any <4G source page (exit 0x91), so every booter buffer must
 * sit above 4 GiB. We walk 16 MiB-aligned slots from a moving cursor, fall
 * back to three static candidates, and only as a last resort go below 4 G.
 * The returned physical address is directly dereferenceable because UEFI
 * identity-maps all DRAM (same assumption dma_alloc_below_4g already makes).
 * Returns 0 on total failure. Exported so booter.c can call it. */
static uint64_t booter_high_cursor = 0x110000000ULL;

UINT64 booter_alloc_high(UINTN pages)
{
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS address;
    static const EFI_PHYSICAL_ADDRESS candidates[] = {
        0x1A0000000ULL, 0x1C0000000ULL, 0x1E0000000ULL,
    };
    UINTN i;

    if (!g_boot_services || !pages)
        return 0;

    for (i = 0; i < 12; i++) {
        address = booter_high_cursor + (uint64_t)i * 0x1000000ULL;
        status = g_boot_services->AllocatePages(AllocateAddress,
                    EfiReservedMemoryType, pages, &address);
        if (!EFI_ERROR(status)) {
            booter_high_cursor =
                address + ((uint64_t)pages << 12) + 0x1000000ULL;
            return (UINT64)address;
        }
    }

    for (i = 0; i < ARRAY_SIZE(candidates); i++) {
        address = candidates[i];
        status = g_boot_services->AllocatePages(AllocateAddress,
                    EfiReservedMemoryType, pages, &address);
        if (!EFI_ERROR(status))
            return (UINT64)address;
    }

    /* Last resort: a plain below-4G page allocation. On the locked 40HX the
     * booter will reject this (mbox exit 0x91) but the caller still gets a
     * coherent, logged failure instead of a silent 0. */
    address = 0xffffffffULL;
    status = g_boot_services->AllocatePages(AllocateMaxAddress,
                EfiReservedMemoryType, pages, &address);
    return EFI_ERROR(status) ? 0 : (UINT64)address;
}

static void SYSVABI dma_free(struct gpu_dev *g, void *memory, size_t size,
                             uint64_t physical)
{
    UINTN pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    (void)g;
    (void)memory;
    if (physical && pages)
        g_boot_services->FreePages(physical, pages);
}

static int SYSVABI pci_cfg_read32(struct gpu_dev *g, unsigned offset,
                                  uint32_t *value)
{
    struct platform_gpu *platform = (struct platform_gpu *)g->platform_data;
    return EFI_ERROR(pci_read(&platform->gpu, offset,
                              EfiPciWidthUint32, value)) ? -1 : 0;
}

static void SYSVABI delay_ms(unsigned milliseconds)
{
    g_boot_services->Stall((UINTN)milliseconds * 1000U);
}

static int sbr_once(struct gpu_dev *g, UINT32 settle_ms)
{
    struct platform_gpu *platform = (struct platform_gpu *)g->platform_data;
    UINT16 bridge_control;
    UINT16 asserted;
    UINT16 restored;
    UINT32 id = 0xffffffffU;
    UINTN attempt;

    if (EFI_ERROR(pci_read(&platform->bridge, PCI_BRIDGE_CONTROL_OFFSET,
                           EfiPciWidthUint16, &bridge_control)))
        return -1;
    asserted = bridge_control | PCI_BRIDGE_CTL_BUS_RESET;
    restored = bridge_control & ~PCI_BRIDGE_CTL_BUS_RESET;
    if (EFI_ERROR(pci_write(&platform->bridge, PCI_BRIDGE_CONTROL_OFFSET,
                            EfiPciWidthUint16, &asserted)))
        return -1;
    g_boot_services->Stall(100000U);
    if (EFI_ERROR(pci_write(&platform->bridge, PCI_BRIDGE_CONTROL_OFFSET,
                            EfiPciWidthUint16, &restored)))
        return -1;
    g_boot_services->Stall(1000000U);
    if (restore_gpu_config(platform))
        return -1;
    if (settle_ms)
        g_boot_services->Stall((UINTN)settle_ms * 1000U);

    /* Compare against the identity captured during the initial PCI scan.
     * A CMP40HX reports 10de:1f0b, not the 90HX ID hard-coded by upstream.
     * Poll briefly as some firmware needs extra time to re-enable the link. */
    for (attempt = 0; attempt < 100; attempt++) {
        id = 0xffffffffU;
        if (!EFI_ERROR(pci_read(&platform->gpu, 0, EfiPciWidthUint32, &id)) &&
            id != 0xffffffffU && id != 0)
            break;
        g_boot_services->Stall(100000U);
    }
    if (id != platform->expected_id) {
        gpu_log(g, "SBR identity check failed: expected %08x, read %08x\n",
                platform->expected_id, id);
        return -1;
    }
    gpu_log(g, "SBR device ready: %08x\n", id);
    return 0;
}

static int SYSVABI secondary_bus_reset(struct gpu_dev *g)
{
    gpu_log(g, "SBR via %04x:%02x:%02x.%u; settling 3 seconds\n",
            ((struct platform_gpu *)g->platform_data)->bridge.segment,
            ((struct platform_gpu *)g->platform_data)->bridge.bus,
            ((struct platform_gpu *)g->platform_data)->bridge.device,
            ((struct platform_gpu *)g->platform_data)->bridge.function);
    /* The fixed settle below is only a conservative pad after the link is
     * re-enabled; the real readiness gate is the device-ID poll inside
     * sbr_once (up to 100 x 100 ms, breaking as soon as 10de:1f0b returns).
     * 3 s is enough on this board and saves ~5 s per boot versus the old 8 s. */
    return sbr_once(g, 3000U);
}

static UINT32 mmio_rd(uintptr_t base, UINT32 off)
{
    return *(volatile UINT32 *)(base + off);
}

static void mmio_rmw(uintptr_t base, UINT32 off, UINT32 clear, UINT32 set)
{
    volatile UINT32 *reg = (volatile UINT32 *)(base + off);
    *reg = (*reg & ~clear) | set;
}

/* Walk the standard capability list to find the PCI Express capability (0x10). */
static int find_pcie_cap(const struct pci_location *loc, UINT32 *off)
{
    UINT8 next = 0;
    UINT32 guard = 0;
    if (EFI_ERROR(pci_read(loc, PCI_CAPABILITY_LIST, EfiPciWidthUint8, &next)))
        return -1;
    while (next >= 0x40U && next < 0xffU && guard++ < 48U) {
        UINT8 id = 0;
        if (EFI_ERROR(pci_read(loc, next, EfiPciWidthUint8, &id)))
            return -1;
        if (id == PCI_CAP_ID_EXP) {
            *off = next;
            return 0;
        }
        if (EFI_ERROR(pci_read(loc, (UINT32)(next + 1U), EfiPciWidthUint8, &next)))
            return -1;
    }
    return -1;
}

/* Print the whole link state of one end of the link. The UEFI screen then
 * carries the same evidence as `lspci -vv` on the running system, which is
 * what every previous attempt in this project was missing. */
static void dump_link_state(const struct pci_location *loc, UINT32 cap,
                            const char *tag)
{
    UINT32 lnkcap = 0;
    UINT32 lnkcap2 = 0;
    UINT16 lnkctl = 0;
    UINT16 lnksta = 0;
    UINT16 lnkctl2 = 0;

    (void)pci_read(loc, cap + PCI_EXP_LNKCAP, EfiPciWidthUint32, &lnkcap);
    (void)pci_read(loc, cap + PCI_EXP_LNKCAP2, EfiPciWidthUint32, &lnkcap2);
    (void)pci_read(loc, cap + PCI_EXP_LNKCTL, EfiPciWidthUint16, &lnkctl);
    (void)pci_read(loc, cap + PCI_EXP_LNKSTA, EfiPciWidthUint16, &lnksta);
    (void)pci_read(loc, cap + PCI_EXP_LNKCTL2, EfiPciWidthUint16, &lnkctl2);
    console_printf("gen2 %s CAP=%08x CAP2=%08x CTL=%04x CTL2=%04x STA=%04x spd=%u\n",
                   tag, lnkcap, lnkcap2, lnkctl, lnkctl2, lnksta,
                   (unsigned)(lnksta & PCI_EXP_LNKSTA_SPEED_MASK));
}

static int link_is_gen2(const struct pci_location *loc, UINT32 cap)
{
    UINT16 sta = 0;
    if (EFI_ERROR(pci_read(loc, cap + PCI_EXP_LNKSTA, EfiPciWidthUint16, &sta)))
        return 0;
    return (sta & PCI_EXP_LNKSTA_SPEED_MASK) == 0x0002U;
}

/* Apply the empirically verified CMP40HX PCIe Gen2 policy and retrain the
 * link. This runs AFTER the compute/full SBR sequence, because a bus reset
 * clears the endpoint Link Control 2 target speed back to Gen1. It is
 * deliberately non-fatal: a Gen2 promotion that the OS driver later reverts
 * must never turn a successful compute unlock into an error return.
 *
 * Two bits matter beyond the target speed itself and were never set before:
 *   LnkCtl2.HASD  - stop the link hardware from walking the speed back down
 *   LnkCtl.HAWD   - same for the width
 * One upstream LnkSta.LBMS (bit14) means a bandwidth change already happened
 * and the port settled at 2.5GT/s; it is cleared so the next poll is clean. */
static void apply_gen2(struct gpu_dev *g)
{
    struct platform_gpu *platform = (struct platform_gpu *)g->platform_data;
    uintptr_t mmio = (uintptr_t)g->mmio;
    UINT32 gpu_cap = 0;
    UINT32 br_cap = 0;
    UINT16 value = 0;
    UINT16 root_ctl = 0;
    int have_root_ctl;
    UINTN attempt;
    UINTN round;

    console_write("\n== gen2 ==\n");

    /* 1. Materialise the policy. Writing CFG/PL is what turns the endpoint's
     *    LnkCap/LnkCap2 from "2.5GT/s only" into the 2.5-5GT/s range, so it has
     *    to happen even if the capability walk below fails. */
    mmio_rmw(mmio, GEN2_OVR_OFF, 0xffffffffU, 0x00000006U);
    mmio_rmw(mmio, GEN2_CFG_OFF, 0x000c0000U, 0x00080000U);
    mmio_rmw(mmio, GEN2_PL_OFF, 0x00060000U, 0x00040000U);
    mmio_rmw(mmio, GEN2_MISC_OFF, 0x00005000U, 0x00002800U);
    mmio_rmw(mmio, GEN2_CYA_OFF, 0x00000004U, 0x00000000U);
    console_printf("gen2 policy: OVR=%08x CFG=%08x PL=%08x MISC=%08x CYA=%08x\n",
                   mmio_rd(mmio, GEN2_OVR_OFF), mmio_rd(mmio, GEN2_CFG_OFF),
                   mmio_rd(mmio, GEN2_PL_OFF), mmio_rd(mmio, GEN2_MISC_OFF),
                   mmio_rd(mmio, GEN2_CYA_OFF));

    if (find_pcie_cap(&platform->gpu, &gpu_cap) ||
        find_pcie_cap(&platform->bridge, &br_cap)) {
        console_write("gen2: PCIe capability not found on GPU or upstream; skipped.\n");
        return;
    }

    /* The GPU publishes the new capability through its own policy engine, not
     * synchronously with the MMIO write; sample after a short settle. */
    g_boot_services->Stall(200000U);
    dump_link_state(&platform->gpu, gpu_cap, "gpu  policy");
    dump_link_state(&platform->bridge, br_cap, "root pre   ");

    /* 2. Target 5GT/s on both ends and forbid autonomous down-training. */
    if (!EFI_ERROR(pci_read(&platform->gpu, gpu_cap + PCI_EXP_LNKCTL2,
                            EfiPciWidthUint16, &value))) {
        value = (UINT16)((value & ~PCI_EXP_LNKCTL2_TLS_MASK) | 0x0002U |
                         PCI_EXP_LNKCTL2_HASD);
        pci_write(&platform->gpu, gpu_cap + PCI_EXP_LNKCTL2,
                  EfiPciWidthUint16, &value);
    }
    if (!EFI_ERROR(pci_read(&platform->bridge, br_cap + PCI_EXP_LNKCTL2,
                            EfiPciWidthUint16, &value))) {
        value = (UINT16)((value & ~PCI_EXP_LNKCTL2_TLS_MASK) | 0x0002U |
                         PCI_EXP_LNKCTL2_HASD);
        pci_write(&platform->bridge, br_cap + PCI_EXP_LNKCTL2,
                  EfiPciWidthUint16, &value);
    }

    /* 3. Common Clock Configuration has to match on both ends. When it does
     *    not, a Gen2 retrain finishes with no TrainingError and still settles
     *    back at 2.5GT/s. This board declares common clock on the root port
     *    (CTL=0x0042) and separate clock on the GPU (CTL=0x0000) while both
     *    say slot clock = 1 - the root port's CCC bit is misleading here.
     *    Measured on CMP40HX + Xeon E7 v4 IIO: both ends = 1 never trains,
     *    both ends = 0 trains Gen2 x16 on the first retrain. So separate
     *    clock is tried first, and mirroring the root port is the fallback
     *    for platforms that really do share a reference clock. */
    have_root_ctl = !EFI_ERROR(pci_read(&platform->bridge,
                                        br_cap + PCI_EXP_LNKCTL,
                                        EfiPciWidthUint16, &root_ctl));
    for (round = 0; round < 2; round++) {
        UINT16 ccc = (round == 0 || !have_root_ctl)
                         ? 0 : (UINT16)(root_ctl & PCI_EXP_LNKCTL_CCC);

        /* ASPM off as well: an L1 link can refuse to come back up at 5GT/s. */
        if (!EFI_ERROR(pci_read(&platform->gpu, gpu_cap + PCI_EXP_LNKCTL,
                                EfiPciWidthUint16, &value))) {
            value = (UINT16)((value & ~(PCI_EXP_LNKCTL_ASPM |
                                        PCI_EXP_LNKCTL_CCC)) | ccc);
            pci_write(&platform->gpu, gpu_cap + PCI_EXP_LNKCTL,
                      EfiPciWidthUint16, &value);
        }
        if (!EFI_ERROR(pci_read(&platform->bridge, br_cap + PCI_EXP_LNKCTL,
                                EfiPciWidthUint16, &value))) {
            value = (UINT16)((value & ~(PCI_EXP_LNKCTL_ASPM |
                                        PCI_EXP_LNKCTL_CCC)) | ccc |
                             PCI_EXP_LNKCTL_HAWD | PCI_EXP_LNKCTL_RETRAIN);
            pci_write(&platform->bridge, br_cap + PCI_EXP_LNKCTL,
                      EfiPciWidthUint16, &value);
        }
        /* LnkSta.LBMS is RW1C: clear the stale "bandwidth changed" flag. */
        if (!EFI_ERROR(pci_read(&platform->bridge, br_cap + PCI_EXP_LNKSTA,
                                EfiPciWidthUint16, &value))) {
            value = (UINT16)(value | PCI_EXP_LNKSTA_LBMS);
            pci_write(&platform->bridge, br_cap + PCI_EXP_LNKSTA,
                      EfiPciWidthUint16, &value);
        }
        console_printf("gen2: retrain round %u, CCC=%u on both ends\n",
                       (unsigned)(round + 1U), (unsigned)(ccc ? 1U : 0U));
        dump_link_state(&platform->gpu, gpu_cap, "gpu  tgt   ");
        dump_link_state(&platform->bridge, br_cap, "root tgt   ");

        for (attempt = 0; attempt < 20; attempt++) {
            g_boot_services->Stall(100000U);
            if (link_is_gen2(&platform->gpu, gpu_cap) &&
                link_is_gen2(&platform->bridge, br_cap))
                break;
        }
        dump_link_state(&platform->gpu, gpu_cap, "gpu  done  ");
        dump_link_state(&platform->bridge, br_cap, "root done  ");
        if (link_is_gen2(&platform->gpu, gpu_cap) &&
            link_is_gen2(&platform->bridge, br_cap))
            break;
    }
    if (link_is_gen2(&platform->gpu, gpu_cap))
        console_write("gen2: link trained at Gen2 during the UEFI stage.\n");
    else if (have_root_ctl) {
        /* Neither clock mode promoted the link: hand the upstream port back
         * exactly as we found it so the OS enumerates the same link it would
         * have without us. The GPU side already matches its power-on state. */
        value = root_ctl;
        pci_write(&platform->bridge, br_cap + PCI_EXP_LNKCTL,
                  EfiPciWidthUint16, &value);
        console_write("gen2: still Gen1; upstream Link Control restored.\n");
    } else {
        console_write("gen2: still Gen1 after both clock modes (see the dumps).\n");
    }
}

static int has_word(const CHAR16 *options, UINTN bytes, const char *word)
{
    UINTN chars = bytes / sizeof(CHAR16);
    UINTN i;
    for (i = 0; i < chars; i++) {
        UINTN j = 0;
        if (i && options[i - 1] > ' ')
            continue;
        while (word[j] && i + j < chars &&
               options[i + j] == (CHAR16)(UINT8)word[j])
            j++;
        if (!word[j] && (i + j == chars || options[i + j] <= ' '))
            return 1;
    }
    return 0;
}

static enum requested_mode parse_mode(EFI_HANDLE image_handle)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    if (EFI_ERROR(g_boot_services->HandleProtocol(
            image_handle, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID,
            (void **)&loaded)) || !loaded || !loaded->LoadOptions) {
        /* No LoadOptions (UEFI boot entry / GRUB chainloader). */
#ifdef AUTO_MODE
        /* Compiled with -DAUTO_MODE=REQUEST_xxx (GRUB integration). */
        console_write("No LoadOptions; auto mode enabled (build flag).\n");
        return (enum requested_mode)AUTO_MODE;
#else
        /* Safe read-only fallback. */
        return REQUEST_INSPECT;
#endif
    }
    if (has_word((const CHAR16 *)loaded->LoadOptions,
                 loaded->LoadOptionsSize, "booter"))
        return REQUEST_BOOTER;
    return REQUEST_INSPECT;
}

static int run_one(struct core_image *core, struct gpu_dev *gpu, int mode,
                   const char *name)
{
    int result;
    console_printf("\n== %s ==\n", name);
    result = core->do_permissive(gpu, mode);
    if (result) {
        console_printf("%s failed: %d\n", name, result);
        return result;
    }
    console_printf("%s complete\n", name);
    return 0;
}

static int run_requested(enum requested_mode request, struct core_image *core,
                         struct gpu_dev *gpu)
{
    if (request == REQUEST_EXIT)
        return 0;
    if (request == REQUEST_INSPECT)
        return run_one(core, gpu, NVPERM_MODE_GR_INSPECT, "read-only inspect");
    if (request == REQUEST_BOOTER) {
        /* DIRECT_SEC2 compute unlock: driven entirely by the main EFI
         * image (booter.c), not the core ELF. booter_run() first issues one
         * normalize SBR (so a BIOS- or earlier-attempt-latched WPR2 cannot
         * wedge the port-load), does its engine reset, and on the success path
         * issues no further SBR - the SS0/SS1 the booter just wrote survive
         * into the Gen2 + ReBAR tail below, so a single boot delivers compute
         * unlock, Gen2 link and the 8 GiB BAR. ReBAR stays last because it is
         * the step that must follow the last bus reset. */
        int r;
        console_write("\n== booter (DIRECT_SEC2) ==\n");
        r = booter_run(gpu);
        if (r) {
            console_write("booter: DIRECT_SEC2 did not reach UNLOCKED; "
                          "Gen2 and ReBAR skipped (link and BAR left as-is).\n");
            /* The fwsec step latched WPR2 even though the booter did not
             * unlock. Leaving it set makes the OS driver abort its GSP boot
             * ("unexpected WPR2 already up" -> nvidia-smi: No devices were
             * found). Reset the endpoint so the driver instead binds a clean,
             * locked CMP 40HX: the worst case becomes "not boosted", never
             * "device missing". This is the in-band version of the old
             * "power the machine fully off before retrying" instruction. */
            console_write("booter: cleanup SBR to clear latched WPR2 for the OS driver...\n");
            if (secondary_bus_reset(gpu))
                console_write("booter: cleanup SBR failed; a full power-off is required.\n");
            return r;
        }
        apply_gen2(gpu);
        if (run_one(core, gpu, NVPERM_MODE_REBAR, "rebar"))
            console_write("rebar unlock failed; BAR1 stays 64 MiB (harmless).\n");
        return 0;
    }

    /* REQUEST_FULL / COMPUTE / GRAPHICS / GR_* were retired once the booter
     * one-shot covered the whole chain; an unknown word decodes as
     * REQUEST_UNKNOWN here and inspect is already handled above. */
    console_write("Unknown mode; nothing executed.\n");
    return -1;
}

/* v29.67: interactive menu + key-wait so the user can SEE the tool running
 * (previously: booting the stick fell through to Windows - tool never ran).
 * 等待按键的时长: 1 tick = Stall(10us), 500000 ticks = 5 秒。
 * 想改长/改短只改这一个数 (N 秒 -> N*100000)。 */
#define WAIT_KEY_TICKS 500000U

static int wait_any_key(void)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin =
        (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)g_system_table->ConIn;
    EFI_INPUT_KEY key;
    UINTN i;
    for (i = 0; i < WAIT_KEY_TICKS; i++) {
        if (conin && conin->ReadKeyStroke &&
            !EFI_ERROR(conin->ReadKeyStroke(conin, &key)))
            return 1;
        if (g_boot_services && g_boot_services->Stall)
            g_boot_services->Stall(10);
    }
    return 0;
}

static enum requested_mode interactive_menu(void)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin =
        (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)g_system_table->ConIn;
    EFI_INPUT_KEY key;
    console_write("\n======== CMP40HX UEFI Unlock Tool (v29.68) ========\n");
    console_write("  Target : NVIDIA 10de:1f0b (CMP 40HX)\n");
    console_write("  [1] inspect  - read-only check (safe, run this first)\n");
    console_write("  [2] boot PVE/Linux now (do nothing)\n");
    console_write("  [3] booter  - one-shot: DIRECT_SEC2 compute unlock + Gen2 + ReBAR (v70 port)\n");
    console_write("\n  Select mode: ");
    for (;;) {
        if (conin && conin->ReadKeyStroke &&
            !EFI_ERROR(conin->ReadKeyStroke(conin, &key))) {
            if (key.UnicodeChar == L'1')
                return REQUEST_INSPECT;
            if (key.UnicodeChar == L'2' || key.ScanCode == 0x17)
                return REQUEST_EXIT;
            if (key.UnicodeChar == L'3')
                return REQUEST_BOOTER;
            if (key.UnicodeChar == 0x0D)
                return REQUEST_INSPECT;
        }
        if (g_boot_services && g_boot_services->Stall)
            g_boot_services->Stall(1000);
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    struct platform_gpu platform;
    struct core_image core;
    struct gpu_dev gpu;
    enum requested_mode request;
    UINT32 boot0;
    EFI_STATUS core_status;
    int matches;
    int result;

    g_system_table = system_table;
    g_boot_services = system_table ? system_table->BootServices : 0;
    if (!g_boot_services)
        return EFI_INVALID_PARAMETER;
    trace_open(image_handle);   /* mirror all console_write to ESP for hang diagnosis */
    memset(&platform, 0, sizeof(platform));
    memset(&core, 0, sizeof(core));
    memset(&gpu, 0, sizeof(gpu));
    request = parse_mode(image_handle);

    /* v29.67: interactive menu when launched from the boot menu without
     * command-line args (stick is now bootable via \EFI\BOOT\BOOTX64.EFI). */
    {
        EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
        EFI_STATUS ist = g_boot_services->HandleProtocol(
            image_handle, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID,
            (void **)&img);
        if (EFI_ERROR(ist) || !img || img->LoadOptionsSize == 0)
            request = interactive_menu();
    }

    console_write("CMP40HX Unlock / UEFI proof of concept\n");
    console_write("Target: NVIDIA 10de:1f0b / 10de:220d. Default mode is read-only inspect.\n\n");

    if (request == REQUEST_EXIT) {
        console_write("No action requested. Boot Linux.\n");
        trace_close();
        return EFI_SUCCESS;
    }

    matches = find_cmp90hx(&platform);
    if (matches != 1) {
        console_printf("Refusing to continue: found %d CMP 90HX devices; expected exactly one.\n",
                       matches);
        return EFI_NOT_FOUND;
    }
    if (find_upstream_bridge(&platform)) {
        console_write("Could not identify the immediate upstream PCI bridge.\n");
        return EFI_NOT_FOUND;
    }
    if (prepare_pci_resources(&platform)) {
        console_write("BAR discovery or PCI command setup failed.\n");
        return EFI_DEVICE_ERROR;
    }

    console_printf("GPU %04x:%02x:%02x.%u ID=%08x\n",
                   platform.gpu.segment, platform.gpu.bus,
                   platform.gpu.device, platform.gpu.function,
                   platform.expected_id);
    console_printf("Upstream bridge %04x:%02x:%02x.%u\n",
                   platform.bridge.segment, platform.bridge.bus,
                   platform.bridge.device, platform.bridge.function);
    console_printf("BAR0=0x%llx BAR1=0x%llx\n",
                   (unsigned long long)platform.bar0,
                   (unsigned long long)platform.bar1);

    boot0 = ioread32((const volatile void *)(uintptr_t)
                     (platform.bar0 + NV_PMC_BOOT_0));
    console_printf("BOOT0=0x%08x\n", boot0);
    /* 40HX port: GA102 strict check OR Turing-family check (TU106 boot0 arch
     * is not confidently known on this sample - observed 0x0FFFFFFF readback
     * pattern on the 40HX; accept GA10X, TU10X, or the degenerate all-ones
     * readback, otherwise refuse like upstream). */
    if ((boot0 & BOOT0_ARCH_MASK) != BOOT0_ARCH_GA10X &&
        (boot0 & BOOT0_ARCH_MASK) != BOOT0_ARCH_TU10X &&
        (boot0 & BOOT0_ARCH_MASK) != BOOT0_ARCH_TU106 &&
        (boot0 & 0xff000000U) != 0x0f000000U &&
        boot0 != 0x0fffffffU) {
        console_write("BOOT0 is not GA102/TU106. No register write was attempted.\n");
        return EFI_UNSUPPORTED;
    }

    core_status = load_nvpermissive_core(
        g_boot_services,
        _binary_nvpermissive_core_o_start,
        (UINTN)(_binary_nvpermissive_core_o_end -
                _binary_nvpermissive_core_o_start),
        &core);
    if (EFI_ERROR(core_status)) {
        console_printf("Core ELF loader failed: 0x%llx\n",
                       (unsigned long long)core_status);
        return core_status;
    }
    console_printf("Core loaded below 2 GiB: %s\n",
                   core.version ? core.version : "version unavailable");

    gpu.mmio = (volatile void *)(uintptr_t)platform.bar0;
    gpu.pci_device_id = (uint16_t)(platform.expected_id >> 16);
    gpu.fb_mmio = (void *)(uintptr_t)platform.bar1;
    gpu.fb_base = 0;
    gpu.fb_map_size = FB_WINDOW_SIZE;
    gpu.host_alloc = host_alloc;
    gpu.host_free = host_free;
    gpu.dma_alloc = dma_alloc_below_4g;
    gpu.dma_free = dma_free;
    gpu.dma_alloc_low = dma_alloc_below_4g;
    gpu.pci_cfg_read32 = pci_cfg_read32;
    gpu.delay_ms = delay_ms;
    gpu.secondary_bus_reset = secondary_bus_reset;
    gpu.log = gpu_log;
    gpu.platform_data = &platform;

    result = run_requested(request, &core, &gpu);
    if (result) {
        console_printf("\nFAILED (%d). Power the machine fully off before retrying.\n",
                       result);
        trace_close();
        return EFI_DEVICE_ERROR;
    }
    if (request == REQUEST_INSPECT) {
        console_write("\nInspect completed. No unlock mode was requested.\n");
    } else {
        console_write("\nUNLOCK SEQUENCE COMPLETED.\n");
        console_write("Boot Linux without powering off.\n");
    }
    console_write("\nPress any key to continue booting PVE/Linux (auto in 5s)...\n");
    wait_any_key();
    trace_close();
    return EFI_SUCCESS;
}
