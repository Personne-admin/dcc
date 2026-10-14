#include <qemu-plugin.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
struct entry { uint64_t pc; _Atomic uint64_t count; };
static struct entry *entries;
static size_t length;
static const char *output;

static void executed(unsigned int cpu, void *data)
{
    (void)cpu;
    struct entry *entry = data;
    atomic_fetch_add_explicit(&entry->count, 1, memory_order_relaxed);
}

static void translated(struct qemu_plugin_tb *tb, void *unused)
{
    (void)unused;
    for (size_t i = 0; i < qemu_plugin_tb_n_insns(tb); ++i) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        size_t low = 0, high = length;
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (entries[mid].pc < pc) low = mid + 1;
            else high = mid;
        }
        if (low < length && entries[low].pc == pc)
            qemu_plugin_register_vcpu_insn_exec_cb(insn, executed, QEMU_PLUGIN_CB_NO_REGS, &entries[low]);
    }
}

static void finished(void *unused)
{
    (void)unused;
    FILE *file = fopen(output, "w");
    if (!file) abort();
    for (size_t i = 0; i < length; ++i)
        fprintf(file, "%" PRIx64 " %" PRIu64 "\n", entries[i].pc, atomic_load(&entries[i].count));
    if (fclose(file)) abort();
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    (void)info;
    if (argc != 2 || strncmp(argv[0], "map=", 4) || strncmp(argv[1], "out=", 4)) return -1;
    FILE *file = fopen(argv[0] + 4, "r");
    if (!file) return -1;
    output = argv[1] + 4;
    size_t capacity = 256;
    entries = calloc(capacity, sizeof(*entries));
    if (!entries) abort();
    uint64_t pc;
    while (fscanf(file, "%" SCNx64, &pc) == 1) {
        if (length && pc <= entries[length - 1].pc) abort();
        if (length == capacity) {
            capacity *= 2;
            entries = realloc(entries, capacity * sizeof(*entries));
            if (!entries) abort();
        }
        entries[length].pc = pc;
        atomic_init(&entries[length++].count, 0);
    }
    fclose(file);
    qemu_plugin_register_vcpu_tb_trans_cb(id, translated, NULL);
    qemu_plugin_register_atexit_cb(id, finished, NULL);
    return 0;
}
