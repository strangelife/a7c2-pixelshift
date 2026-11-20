/* AArch64 CRC-32 (IEEE), selected only if Linux advertises the extension. */
static int crc32_available(void) {
    enum { AT_HWCAP = 16, HWCAP_CRC32 = 1 << 7 };
    i64 fd = sc(56, -100, (i64)"/proc/self/auxv", 0, 0, 0, 0);
    if (fd < 0) return 0;
    u64 entries[128];
    i64 size = sc(63, fd, (i64)entries, sizeof(entries), 0, 0, 0);
    i64 closed = sc(57, fd, 0, 0, 0, 0, 0);
    if (closed || size < 16 || size > (i64)sizeof(entries) || size % 16) return 0;
    for (i64 i = 0; i < size / 8 && entries[i]; i += 2)
        if (entries[i] == AT_HWCAP) return (entries[i + 1] & HWCAP_CRC32) != 0;
    return 0;
}

__attribute__((target("crc")))
static u32 crc32_arm(u32 value, const u8 *data, u32 size) {
    while (size >= 8) {
        u64 word;
        __builtin_memcpy(&word, data, sizeof(word));
        __asm__("crc32x %w0, %w0, %x1" : "+r" (value) : "r" (word));
        data += 8;
        size -= 8;
    }
    while (size--) {
        u32 byte = *data++;
        __asm__("crc32b %w0, %w0, %w1" : "+r" (value) : "r" (byte));
    }
    return value;
}
