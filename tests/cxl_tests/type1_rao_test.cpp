#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "virt2phy.h"

namespace
{

constexpr off_t kMapSize = 1 << 20;
constexpr uint64_t kCtrl = 0x00;
constexpr uint64_t kStatus = 0x08;
constexpr uint64_t kStageSeqId = 0x28;
constexpr uint64_t kStageGroupId = 0x30;
constexpr uint64_t kStageStepId = 0x38;
constexpr uint64_t kStagePaddr = 0x40;
constexpr uint64_t kStageOpcode = 0x48;
constexpr uint64_t kStageOperandMode = 0x50;
constexpr uint64_t kStageOperandImm = 0x58;
constexpr uint64_t kStageResultId = 0x60;
constexpr uint64_t kPushTrace = 0x68;
constexpr uint64_t kCtrlStart = 1;
constexpr uint64_t kStatusDone = 1 << 1;

struct TraceEntry {
    uint64_t seq_id = 0;
    uint64_t group_id = 0;
    uint64_t step_id = 0;
    uint64_t addr_offset = 0;
    uint32_t data_size = 8;
    uint32_t opcode = 0;
    uint32_t operand_mode = 0;
    uint64_t operand_imm = 0;
    int64_t result_id = -1;
};

struct InitValue {
    uint64_t offset = 0;
    uint64_t value = 0;
};

struct MetaInfo {
    uint64_t replay_buffer_size = 0;
    uint64_t default_init_value = 0;
    std::vector<InitValue> init_values;
};

uint64_t
parseUint64(const std::string &s)
{
    return std::strtoull(s.c_str(), nullptr, 10);
}

std::vector<std::string>
splitCsv(const std::string &line)
{
    std::vector<std::string> result;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ',')) {
        result.push_back(item);
    }
    return result;
}

std::string
readFile(const char *path)
{
    std::ifstream in(path);
    if (!in.is_open()) {
        return "";
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool
parseJsonUint(const std::string &json, const std::string &key,
              uint64_t *value)
{
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = json.find(quoted_key);
    if (pos == std::string::npos) {
        return false;
    }

    pos = json.find(':', pos + quoted_key.size());
    if (pos == std::string::npos) {
        return false;
    }

    ++pos;
    while (pos < json.size() && isspace(json[pos])) {
        ++pos;
    }

    char *end = nullptr;
    *value = std::strtoull(json.c_str() + pos, &end, 10);
    return end != json.c_str() + pos;
}

bool
parseInitValues(const std::string &json, std::vector<InitValue> *values)
{
    const std::string key = "\"init_values\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        return true;
    }

    pos = json.find('[', pos + key.size());
    if (pos == std::string::npos) {
        return false;
    }

    const size_t end_pos = json.find(']', pos);
    if (end_pos == std::string::npos) {
        return false;
    }

    while (pos < end_pos) {
        size_t obj_start = json.find('{', pos);
        if (obj_start == std::string::npos || obj_start > end_pos) {
            break;
        }
        size_t obj_end = json.find('}', obj_start);
        if (obj_end == std::string::npos || obj_end > end_pos) {
            return false;
        }

        const std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
        InitValue init;
        if (!parseJsonUint(obj, "offset", &init.offset) ||
            !parseJsonUint(obj, "value", &init.value)) {
            return false;
        }
        values->push_back(init);
        pos = obj_end + 1;
    }

    return true;
}

std::string
defaultMetaPath(const char *trace_path)
{
    std::string path(trace_path);
    const std::string suffix = ".csv";
    if (path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        path.resize(path.size() - suffix.size());
    }
    path += ".meta.json";
    return path;
}

bool
loadMeta(const char *meta_path, MetaInfo *meta)
{
    const std::string json = readFile(meta_path);
    if (json.empty()) {
        fprintf(stderr, "failed to open meta: %s\n", meta_path);
        return false;
    }

    if (!parseJsonUint(json, "replay_buffer_size",
                       &meta->replay_buffer_size)) {
        fprintf(stderr, "meta missing replay_buffer_size: %s\n", meta_path);
        return false;
    }

    parseJsonUint(json, "default_init_value", &meta->default_init_value);
    if (!parseInitValues(json, &meta->init_values)) {
        fprintf(stderr, "failed to parse init_values: %s\n", meta_path);
        return false;
    }

    return true;
}

std::vector<TraceEntry>
loadTrace(const char *trace_path)
{
    std::ifstream in(trace_path);
    std::string line;
    std::vector<TraceEntry> entries;

    if (!in.is_open()) {
        fprintf(stderr, "failed to open trace: %s\n", trace_path);
        return entries;
    }

    std::getline(in, line);
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto cols = splitCsv(line);
        if (cols.size() < 10) {
            continue;
        }

        TraceEntry entry;
        entry.seq_id = parseUint64(cols[0]);
        entry.group_id = parseUint64(cols[2]);
        entry.step_id = parseUint64(cols[3]);
        entry.addr_offset = parseUint64(cols[4]);
        entry.data_size = static_cast<uint32_t>(parseUint64(cols[5]));
        if (cols[6] == "FETCH") {
            entry.opcode = 1;
        } else if (cols[6] == "FETCH_ADD") {
            entry.opcode = 2;
        } else if (cols[6] == "ADD") {
            entry.opcode = 3;
        }

        if (cols[7] == "NONE") {
            entry.operand_mode = 0;
        } else if (cols[7] == "IMM") {
            entry.operand_mode = 1;
        } else if (cols[7] == "RESULT_REF") {
            entry.operand_mode = 2;
        }

        entry.operand_imm = parseUint64(cols[8]);
        entry.result_id = static_cast<int64_t>(std::strtoll(
            cols[9].c_str(), nullptr, 10));
        entries.push_back(entry);
    }

    return entries;
}

} // namespace

int
main(int argc, char *argv[])
{
    const char *trace_path = argc > 1 ? argv[1] : "/home/test_code/GATHER_ADD_100.csv";
    const std::string inferred_meta_path = defaultMetaPath(trace_path);
    const char *meta_path = argc > 2 ? argv[2] : inferred_meta_path.c_str();

    auto trace = loadTrace(trace_path);
    if (trace.empty()) {
        fprintf(stderr, "trace is empty: %s\n", trace_path);
        return 1;
    }

    MetaInfo meta;
    if (!loadMeta(meta_path, &meta)) {
        return 1;
    }

    uint64_t max_access = 0;
    for (const auto &entry : trace) {
        const uint64_t end = entry.addr_offset + entry.data_size;
        if (end > max_access) {
            max_access = end;
        }
    }
    if (max_access > meta.replay_buffer_size) {
        fprintf(stderr,
                "trace accesses beyond replay buffer: access=%" PRIu64
                " buffer=%" PRIu64 "\n",
                max_access, meta.replay_buffer_size);
        return 1;
    }

    const size_t replay_size = meta.replay_buffer_size;
    auto *buffer = static_cast<uint8_t *>(
        mmap(nullptr, replay_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (buffer == MAP_FAILED) {
        perror("mmap replay buffer");
        return 1;
    }

    for (size_t i = 0; i < replay_size / sizeof(uint64_t); ++i) {
        reinterpret_cast<uint64_t *>(buffer)[i] = meta.default_init_value;
    }
    for (const auto &init : meta.init_values) {
        if (init.offset + sizeof(uint64_t) > replay_size) {
            fprintf(stderr, "init offset out of range: %" PRIu64 "\n",
                    init.offset);
            munmap(buffer, replay_size);
            return 1;
        }
        *reinterpret_cast<uint64_t *>(buffer + init.offset) = init.value;
    }

    for (size_t i = 0; i < replay_size / sizeof(uint64_t); ++i) {
        volatile uint64_t touch = reinterpret_cast<uint64_t *>(buffer)[i];
        (void)touch;
    }

    int fd = open("/dev/cxl_mem0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/cxl_mem0");
        munmap(buffer, replay_size);
        return 1;
    }

    void *map_buf = mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (map_buf == MAP_FAILED) {
        perror("mmap cxl device");
        close(fd);
        munmap(buffer, replay_size);
        return 1;
    }

    auto write_reg = [map_buf](uint64_t offset, uint64_t value) {
        *reinterpret_cast<volatile uint64_t *>(
            static_cast<uint8_t *>(map_buf) + offset) = value;
    };
    auto read_reg = [map_buf](uint64_t offset) -> uint64_t {
        return *reinterpret_cast<volatile uint64_t *>(
            static_cast<uint8_t *>(map_buf) + offset);
    };

    for (const auto &entry : trace) {
        uint64_t vaddr = reinterpret_cast<uint64_t>(buffer) + entry.addr_offset;
        uint64_t paddr = virt2phy2(reinterpret_cast<void *>(vaddr));
        if (paddr == static_cast<uint64_t>(-1)) {
            fprintf(stderr, "virt2phy failed for offset %" PRIu64 "\n",
                    entry.addr_offset);
            munmap(map_buf, kMapSize);
            close(fd);
            munmap(buffer, replay_size);
            return 1;
        }

        write_reg(kStageSeqId, entry.seq_id);
        write_reg(kStageGroupId, entry.group_id);
        write_reg(kStageStepId, entry.step_id);
        write_reg(kStagePaddr, paddr);
        write_reg(kStageOpcode, entry.opcode);
        write_reg(kStageOperandMode, entry.operand_mode);
        write_reg(kStageOperandImm, entry.operand_imm);
        write_reg(kStageResultId, static_cast<uint64_t>(entry.result_id));
        write_reg(kPushTrace, 1);
    }

    write_reg(kCtrl, kCtrlStart);
    while ((read_reg(kStatus) & kStatusDone) == 0) {
        usleep(1000);
    }

    printf("RAO replay completed, ops=%zu, replay_size=%zu\n",
           trace.size(), replay_size);

    munmap(map_buf, kMapSize);
    close(fd);
    munmap(buffer, replay_size);
    return 0;
}
