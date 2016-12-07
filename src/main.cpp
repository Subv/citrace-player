// Copyright 2015 Tony Wasserka
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

#include <3ds.h>

#include "network.h"
#include "citrace.h"
#include "allocator.h"

static uint32_t FCRAMStartVAddr() {
    // TODO: Is there a better way to find this address without hardcoding?
    return 0x14000000;
}

static uint32_t VRAMStartVAddr() {
    // TODO: Is there a better way to find this address without hardcoding?
    return 0x1F000000;
}

/// Physical memory regions as seen from the ARM11
enum {
    /// IO register area
    IO_AREA_PADDR = 0x10100000,
    IO_AREA_SIZE = 0x01000000, ///< IO area size (16MB)
    IO_AREA_PADDR_END = IO_AREA_PADDR + IO_AREA_SIZE,

    /// MPCore internal memory region
    MPCORE_RAM_PADDR = 0x17E00000,
    MPCORE_RAM_SIZE = 0x00002000, ///< MPCore internal memory size (8KB)
    MPCORE_RAM_PADDR_END = MPCORE_RAM_PADDR + MPCORE_RAM_SIZE,

    /// Video memory
    VRAM_PADDR = 0x18000000,
    VRAM_SIZE = 0x00600000, ///< VRAM size (6MB)
    VRAM_PADDR_END = VRAM_PADDR + VRAM_SIZE,

    /// DSP memory
    DSP_RAM_PADDR = 0x1FF00000,
    DSP_RAM_SIZE = 0x00080000, ///< DSP memory size (512KB)
    DSP_RAM_PADDR_END = DSP_RAM_PADDR + DSP_RAM_SIZE,

    /// AXI WRAM
    AXI_WRAM_PADDR = 0x1FF80000,
    AXI_WRAM_SIZE = 0x00080000, ///< AXI WRAM size (512KB)
    AXI_WRAM_PADDR_END = AXI_WRAM_PADDR + AXI_WRAM_SIZE,

    /// Main FCRAM
    FCRAM_PADDR = 0x20000000,
    FCRAM_SIZE = 0x08000000, ///< FCRAM size (128MB)
    FCRAM_PADDR_END = FCRAM_PADDR + FCRAM_SIZE,
};

/// Virtual user-space memory regions
enum {
    /// Where the application text, data and bss reside.
    PROCESS_IMAGE_VADDR = 0x00100000,
    PROCESS_IMAGE_MAX_SIZE = 0x03F00000,
    PROCESS_IMAGE_VADDR_END = PROCESS_IMAGE_VADDR + PROCESS_IMAGE_MAX_SIZE,

    /// Area where IPC buffers are mapped onto.
    IPC_MAPPING_VADDR = 0x04000000,
    IPC_MAPPING_SIZE = 0x04000000,
    IPC_MAPPING_VADDR_END = IPC_MAPPING_VADDR + IPC_MAPPING_SIZE,

    /// Application heap (includes stack).
    HEAP_VADDR = 0x08000000,
    HEAP_SIZE = 0x08000000,
    HEAP_VADDR_END = HEAP_VADDR + HEAP_SIZE,

    /// Area where shared memory buffers are mapped onto.
    SHARED_MEMORY_VADDR = 0x10000000,
    SHARED_MEMORY_SIZE = 0x04000000,
    SHARED_MEMORY_VADDR_END = SHARED_MEMORY_VADDR + SHARED_MEMORY_SIZE,

    /// Maps 1:1 to an offset in FCRAM. Used for HW allocations that need to be linear in physical
    /// memory.
    LINEAR_HEAP_VADDR = 0x14000000,
    LINEAR_HEAP_SIZE = 0x08000000,
    LINEAR_HEAP_VADDR_END = LINEAR_HEAP_VADDR + LINEAR_HEAP_SIZE,

    /// Maps 1:1 to the IO register area.
    IO_AREA_VADDR = 0x1EC00000,
    IO_AREA_VADDR_END = IO_AREA_VADDR + IO_AREA_SIZE,

    /// Maps 1:1 to VRAM.
    VRAM_VADDR = 0x1F000000,
    VRAM_VADDR_END = VRAM_VADDR + VRAM_SIZE,

    /// Maps 1:1 to DSP memory.
    DSP_RAM_VADDR = 0x1FF00000,
    DSP_RAM_VADDR_END = DSP_RAM_VADDR + DSP_RAM_SIZE,

    /// Read-only page containing kernel and system configuration values.
    CONFIG_MEMORY_VADDR = 0x1FF80000,
    CONFIG_MEMORY_SIZE = 0x00001000,
    CONFIG_MEMORY_VADDR_END = CONFIG_MEMORY_VADDR + CONFIG_MEMORY_SIZE,

    /// Usually read-only page containing mostly values read from hardware.
    SHARED_PAGE_VADDR = 0x1FF81000,
    SHARED_PAGE_SIZE = 0x00001000,
    SHARED_PAGE_VADDR_END = SHARED_PAGE_VADDR + SHARED_PAGE_SIZE,

    /// Area where TLS (Thread-Local Storage) buffers are allocated.
    TLS_AREA_VADDR = 0x1FF82000,
    TLS_ENTRY_SIZE = 0x200,

    /// Equivalent to LINEAR_HEAP_VADDR, but expanded to cover the extra memory in the New 3DS.
    NEW_LINEAR_HEAP_VADDR = 0x30000000,
    NEW_LINEAR_HEAP_SIZE = 0x10000000,
    NEW_LINEAR_HEAP_VADDR_END = NEW_LINEAR_HEAP_VADDR + NEW_LINEAR_HEAP_SIZE,
};

static uint32_t PhysicalToVirtualAddress(uint32_t physical_address) {
    if (physical_address == 0) {
        return 0;
    } else if (physical_address >= VRAM_PADDR && physical_address < VRAM_PADDR_END) {
        return physical_address - VRAM_PADDR + VRAM_VADDR;
    } else if (physical_address >= FCRAM_PADDR && physical_address < FCRAM_PADDR_END) {
        return physical_address - FCRAM_PADDR + FCRAMStartVAddr();
    } else if (physical_address >= DSP_RAM_PADDR && physical_address < DSP_RAM_PADDR_END) {
        return physical_address - DSP_RAM_PADDR + DSP_RAM_VADDR;
    } else if (physical_address >= IO_AREA_PADDR && physical_address < IO_AREA_PADDR_END) {
        return physical_address - IO_AREA_PADDR + IO_AREA_VADDR;
    }

    NetworkPrint("Unknown physical address 0x%08x\n", physical_address);
    NetworkExit();
    std::terminate();
    return 0;
}

// Maps each pica register to the set of stateful (inactive) bytes
// E.g. if bits 16-31 of a register are state and the others are active, the array contains the value "0xC = 0b1000+0b0100".
static const std::array<uint8_t, 0x300> pica_register_state_mask = []() {

    std::array<uint8_t, 0x300> ret{};

    // Explicitly initialize stateful registers
    // TODO: We should instead explicitly "un-initialize" active registers instead of explicitly initializing state registers, since the former are less in total.

    ret[0x40] = 0x1; // cull_mode

    // viewport
    ret[0x41] = 0x7;
    ret[0x43] = 0x7;
    ret[0x4d] = 0x7;
    ret[0x4e] = 0x7;

    // VS output attributes
    for (int i = 0; i < 7; ++i)
        ret[0x50 + i] = 0xF;

    // viewport
    ret[0x68] = 0xF;

    // Texture setup
    ret[0x80] = 0x1;
    ret[0x82] = 0xF;
    ret[0x83] = 0xF;
    ret[0x85] = 0xF;
    ret[0x8e] = 0x1;
    ret[0x92] = 0xF;
    ret[0x93] = 0xF;
    ret[0x95] = 0xF;
    ret[0x96] = 0x1;
    ret[0x9a] = 0xF;
    ret[0x9b] = 0xF;
    ret[0x9d] = 0xF;
    ret[0x9e] = 0x1;

    // TEV stage setup
    for (int i = 0; i < 5; ++i) {
        ret[0xc0 + i] = 0xF;
        ret[0xc8 + i] = 0xF;
        ret[0xd0 + i] = 0xF;
        ret[0xd8 + i] = 0xF;
        ret[0xf0 + i] = 0xF;
        ret[0xf8 + i] = 0xF;
    }
    ret[0xe0] = 0xF;
    ret[0xfd] = 0xF;

    // Output merger
    ret[0x100] = 0xF;
    ret[0x101] = 0xF;
    ret[0x102] = 0xF;
    ret[0x103] = 0xF;
    ret[0x104] = 0xF;
    ret[0x106] = 0xF;

    // framebuffer setup
    ret[0x116] = 0xF;
    ret[0x117] = 0xF;
    ret[0x11c] = 0xF;
    ret[0x11d] = 0xF;
    ret[0x11e] = 0xF;

    // vertex attributes
    ret[0x200] = 0xF;
    ret[0x201] = 0xF;
    ret[0x202] = 0xF;
    for (int i = 0; i < 12; ++i) {
        ret[0x203+3*i] = 0xF;
        ret[0x204+3*i] = 0xF;
        ret[0x205+3*i] = 0xF;
    }
    ret[0x227] = 0xF;
    ret[0x228] = 0xF;

    // Trigger draw: 0x22e+0x22f are active!

    // triangle topology
    ret[0x25e] = 0xF;

    // bool and int uniforms
    ret[0x2b0] = 0xF;
    ret[0x2b1] = 0xF;
    ret[0x2b2] = 0xF;
    ret[0x2b3] = 0xF;
    ret[0x2b4] = 0xF;

    // Vertex shader setup
    ret[0x2ba] = 0xF;
    ret[0x2bb] = 0xF;
    ret[0x2bc] = 0xF;

    // float uniforms
    ret[0x2c0] = 0xF;
    // 0x2c1-0x2c8 are active!

    // VS program and swizzle data
    ret[0x2cb] = 0xF;
    // 0x2cc-0x2d4 are active!
    ret[0x2d5] = 0xF;
    // 0x2c6-0x2dd are active!

    return ret;
}();


int main() {
    // TODO: Evaluate if we should map the entire GSP heap manually here

    // TODO: Maybe we should setup a console first and tell the user that we're waiting for a
    //       network connection.

    NetworkInit();

    NetworkPrint("Hello World!\n");

    gspInit();

    // TODO: Add support for streaming the input file over network

    // Open input file; read and check header
    std::ifstream input("sdmc:/citrace.ctf");
    if (!input) {
        NetworkPrint("Failed to open input file sdmc:/citrace.ctf!\n");
        NetworkExit();
        return 1;
    }

    CiTrace::CTHeader header;
    input.read((char*)&header, sizeof(header));
    if (!input) {
        NetworkPrint("Failed to read CiTrace header!\n");
        NetworkExit();
        return 1;
    }

    if (0 != std::memcmp(header.magic, CiTrace::CTHeader::ExpectedMagicWord(), sizeof(header.magic))) {
        NetworkPrint("Invalid magic word: %c%c%c%c\n", header.magic[0], header.magic[1], header.magic[2], header.magic[3]);
        NetworkExit();
        return 1;
    }

    if (header.version != CiTrace::CTHeader::ExpectedVersion()) {
        NetworkPrint("Unsupported CiTrace version %d. This program only supports version %d CiTraces.\n",
                       header.version, CiTrace::CTHeader::ExpectedVersion());
    }

    // Read stream into local memory
    std::vector<CiTrace::CTStreamElement> stream(header.stream_size);
    input.seekg(header.stream_offset);
    for (unsigned i = 0; i < header.stream_size; ++i)
        input.read((char*)&stream[i], sizeof(stream[i]));

    NetworkPrint("Successfully read input file\n");

    // Create command list buffer (aligned to 16 byte)
    std::vector<uint32_t, LinearHeapAllocator<uint32_t>> command_list;

    // Helper lambda function used to queue commands for setting up initial state
    auto SubmitInternalMemory = [&](uint32_t file_offset, uint32_t num_words, uint32_t pica_register_id, bool is_float_uniform) {
        if (0 == num_words)
            return;

        command_list.push_back(0);
        command_list.push_back(pica_register_id | 0xF0000);

        input.seekg(file_offset);

        // TODO: Should assert that the given size fits into a single command

        if (is_float_uniform) {
            // Pack 4 float24 values into 3 uint32_t values

            bool command_written = false;

            for (size_t index = 1; index < num_words / 4; ++index) {
                // Read 4 24-bit values (all of which are 32-bit aligned)
                uint32_t values[4];
                input.read((char*)values, sizeof(values));
                command_list.push_back((values[3] << 8) | ((values[2] >> 16) & 0xFF));
                if (!command_written) {
                    command_list.push_back((pica_register_id + 1) | 0xF0000 | ((num_words / 4 * 3 - 1) << 20));
                    command_written = true;
                }
                command_list.push_back(((values[2] & 0xFFFF) << 16) | ((values[1] >> 8) & 0xFFFF));
                command_list.push_back(((values[1] & 0xFF) << 24) | (values[0] & 0xFFFFFF));
            }
        } else {
            command_list.push_back({});
            input.read((char*)&command_list.back(), sizeof(uint32_t));
            command_list.push_back((pica_register_id + 1) | 0xF0000 | ((num_words - 1) << 20));

            for (size_t index = 1; index < num_words; ++index) {
                command_list.push_back({});
                input.read((char*)&command_list.back(), sizeof(uint32_t));
            }
        }
    };
    for (unsigned i = 0; i < header.initial_state_offsets.default_attributes_size / 4; ++i) {
        command_list.push_back(i);
        command_list.push_back(0x232 | 0xF0000 | (3 << 20)); // followed by 4 float24 values packed into 3 extra uint32_t parameters

        uint32_t values[4];
        input.seekg(header.initial_state_offsets.default_attributes);
        input.read((char*)values, sizeof(values));
        command_list.push_back((values[3] << 8) | ((values[2] >> 16) & 0xFF));
        command_list.push_back(((values[2] & 0xFFFF) << 16) | ((values[1] >> 8) & 0xFFFF));
        command_list.push_back(((values[1] & 0xFF) << 24) | (values[0] & 0xFFFFFF));
    }
    SubmitInternalMemory(header.initial_state_offsets.gs_program_binary,  header.initial_state_offsets.gs_program_binary_size,  0x29b, false);
    SubmitInternalMemory(header.initial_state_offsets.gs_swizzle_data,    header.initial_state_offsets.gs_swizzle_data_size,    0x2a5, false);
    SubmitInternalMemory(header.initial_state_offsets.gs_float_uniforms,  header.initial_state_offsets.gs_float_uniforms_size,  0x290, true);
    SubmitInternalMemory(header.initial_state_offsets.vs_program_binary,  header.initial_state_offsets.vs_program_binary_size,  0x2cb, false);
    SubmitInternalMemory(header.initial_state_offsets.vs_swizzle_data,    header.initial_state_offsets.vs_swizzle_data_size,    0x2d5, false);
    SubmitInternalMemory(header.initial_state_offsets.vs_float_uniforms,  header.initial_state_offsets.vs_float_uniforms_size,  0x2c0, true);

    // Load initial set of pica registers.
    // NOTE: Loading shader data and stuff also needs to be done by writing pica registers,
    //       which is why we set up this state last.
    input.seekg(header.initial_state_offsets.pica_registers);
    unsigned num_pica_registers = std::min<unsigned>(pica_register_state_mask.size(), header.initial_state_offsets.pica_registers_size);
    for (unsigned regid = 0; regid < num_pica_registers; ++regid) {
        uint32_t value;
        input.read((char*)&value, sizeof(value));
        if (pica_register_state_mask[regid] == 0)
            continue;

        command_list.push_back(value);
        command_list.push_back(regid | (pica_register_state_mask[regid] << 16)); // write value with mask
    }

    // Make sure size is a multiple of 16 bytes
    while ((command_list.size() % (16 / sizeof(uint32_t))) != 0) {
        // Repeat previous command (for lack of a better alternative)
        // TODO: Maybe we can come up with something less intrusive?
        command_list.push_back(command_list[command_list.size() - 2]);
        command_list.push_back(command_list[command_list.size() - 2]);
    }

    GSPGPU_FlushDataCache(command_list.data(), command_list.size() * sizeof(uint32_t));


    // Setup initial GPU state
    gfxInitDefault(); // TODO: Setup framebuffer info instead, here!

    NetworkPrint("Command list is located at %08X", (uint32_t)command_list.data());

    NetworkPrint("Initialization done, starting playback now\n");

    while (aptMainLoop()) {
        hidScanInput();
        if(keysDown() & KEY_START)
            break;

        GX_ProcessCommandList(command_list.data(), command_list.size() * sizeof(uint32_t), 1);
        NetworkPrint("Initial playback GPU state setup done\n");
        // TODO: wait for completion of the command list

        // Set up GPU registers - currently limited to the crucial ones
        // TODO: Setup all of them and not just a few ;)
        std::vector<uint32_t> gpu_regs(header.initial_state_offsets.gpu_registers_size);
        input.seekg(header.initial_state_offsets.gpu_registers);
        for (auto& reg : gpu_regs) {
            input.read((char*)&reg, sizeof(reg));
        }
        // Set up command list parameters
        GSPGPU_WriteHWRegs(0x104018E0 - 0x10100000 + 0x1EC00000 - 0x1EB00000, &gpu_regs[0x18E0 / 4], 4);
        GSPGPU_WriteHWRegs(0x104018E8 - 0x10100000 + 0x1EC00000 - 0x1EB00000, &gpu_regs[0x18E8 / 4], 4);

        for (const auto& stream_element : stream) {
            hidScanInput();
            if(keysDown() & KEY_START)
                goto exit;

            switch (stream_element.type) {
            case CiTrace::FrameMarker:
                NetworkPrint("Reached end of current frame\n");
                gfxSwapBuffersGpu();
                gspWaitForVBlank();
                break;

            case CiTrace::MemoryLoad:
                input.seekg(stream_element.memory_load.file_offset);

                if (stream_element.memory_load.physical_address >= VRAM_PADDR && 
                    stream_element.memory_load.physical_address < VRAM_PADDR_END) {
                    // Address lies in VRAM, which we cannot directly write to, so we request DMAs instead
                    // TODO: Make sure we aren't overwriting any data from previous MemoryUpdates here!
                    // TODO: Guard against invalid inputs (e.g. invalid address or size)

                    static const size_t TransferBufferSize = 1024;
                    char* buffer = (char*)linearAlloc(TransferBufferSize);

                    NetworkPrint("Load 0x%x VRAM bytes from file offset 0x%x to 0x%08x (i.e. vaddr 0x%08x)\n",
                                    stream_element.memory_load.size, stream_element.memory_load.file_offset,
                                    stream_element.memory_load.physical_address,
                                    PhysicalToVirtualAddress(stream_element.memory_load.physical_address));

                    // Transfer data in chunks via DMA
                    for (uint32_t remaining = stream_element.memory_load.size,
                         addr = stream_element.memory_load.physical_address;;
                         remaining -= TransferBufferSize, addr += TransferBufferSize) {
                        auto size = std::min<size_t>(TransferBufferSize, remaining);
                        NetworkPrint("-> Transfer 0x%x bytes from 0x%08x to 0x%08x (i.e. vaddr 0x%08x)\n", (int)size, (u32)buffer, addr, PhysicalToVirtualAddress(addr));
                        input.read(buffer, size);
                        GSPGPU_FlushDataCache(buffer, size);

                        GX_RequestDma((u32*)buffer, (u32*)PhysicalToVirtualAddress(addr), size);
                        gspWaitForDMA();

                        if (remaining <= TransferBufferSize)
                            break;
                    }

                    linearFree(buffer);
                } else {
                    NetworkPrint("Load 0x%x bytes from file offset 0x%x to 0x%08x (i.e. vaddr 0x%08x)\n",
                                   stream_element.memory_load.size, stream_element.memory_load.file_offset,
                                   stream_element.memory_load.physical_address,
                                   PhysicalToVirtualAddress(stream_element.memory_load.physical_address));

                    uint8_t* dest = (uint8_t*)PhysicalToVirtualAddress(stream_element.memory_load.physical_address);
                    if (dest == nullptr) {
                        NetworkPrint("That turned out to be an unknown address\n");
                        break;
                    }

                    input.read((char*)dest, stream_element.memory_load.size);
                    GSPGPU_FlushDataCache(dest, stream_element.memory_load.size);
                }
                break;

            case CiTrace::RegisterWrite:
            {
                // TODO: It's not actually possible to write less than a full word via WriteHWRegs!
                int size = [&stream_element]() {
                    switch (stream_element.register_write.size) {
                    case CiTrace::CTRegisterWrite::SIZE_8: return 1;
                    case CiTrace::CTRegisterWrite::SIZE_16: return 2;
                    case CiTrace::CTRegisterWrite::SIZE_32: return 4;
                    case CiTrace::CTRegisterWrite::SIZE_64: return 8;
                    default: return 0;
                    }
                }();

                std::string register_name = [&stream_element]() -> std::string {
                    switch (stream_element.register_write.physical_address) {
                    case 0x1040001C: return "Memory Fill Control 1";
                    case 0x1040002C: return "Memory Fill Control 2";
                    case 0x104018E0: return "Command List Size";
                    case 0x104018E8: return "Command List Address";
                    case 0x104018F0: return "Command List Trigger";
                    default: return "";
                    };
                }();

                uint32_t data = stream_element.register_write.value & 0xFFFFFFFF;
                if (size <= 4) {
                    static const auto debug_strings = std::map<int,const char*>{
                        { 1, "Writing 0x%02x to register 0x%08x%s%s\n" },
                        { 2, "Writing 0x%04x to register 0x%08x%s%s\n" },
                        { 4, "Writing 0x%08x to register 0x%08x%s%s\n" }
                    };
                    NetworkPrint(debug_strings.at(size),
                                   (uint32_t)(stream_element.register_write.value&0xFFFFFFFF),
                                   stream_element.register_write.physical_address - 0x10100000 + 0x1EC00000 - 0x1EB00000,
                                   register_name.empty() ? "" : " <-- ", register_name.c_str());
                } else {
                    NetworkPrint("Writing 0x%08x%08x to register 0x%08x%s%s\n",
                                   (uint32_t)(stream_element.register_write.value>>32),
                                   (uint32_t)(stream_element.register_write.value&0xFFFFFFFF),
                                   stream_element.register_write.physical_address - 0x10100000 + 0x1EC00000 - 0x1EB00000,
                                   register_name.empty() ? "" : " <-- ", register_name.c_str());
                }

                if (stream_element.register_write.physical_address == 0x104018F0) {
                    // Command list processing trigger. Writing directly to the GPU registers here
                    // causes a freeze sometimes (for unknown reasons), so we translate this to a
                    // GX command instead.
                    // TODO: This still doesn't always work on real hardware. We need to figure out
                    //       how to fix the remaining freezes.
                    uint32_t addr = 0;
                    uint32_t size = 0;
                    GSPGPU_ReadHWRegs(0x104018E0 - 0x10100000 + 0x1EC00000 - 0x1EB00000, &size, 4);
                    GSPGPU_ReadHWRegs(0x104018E8 - 0x10100000 + 0x1EC00000 - 0x1EB00000, &addr, 4);
                    GX_ProcessCommandList((u32*)PhysicalToVirtualAddress(addr * 8), size, stream_element.register_write.value);
                } else {
                    GSPGPU_WriteHWRegs(stream_element.register_write.physical_address - 0x10100000 + 0x1EC00000 - 0x1EB00000, &data, size);
                }

                // Wait for completion if the register write triggered an operation
                if (stream_element.register_write.physical_address == 0x1040001C ||
                    stream_element.register_write.physical_address == 0x1040002C ||
                    stream_element.register_write.physical_address == 0x10400C18 ||
                    stream_element.register_write.physical_address == 0x104018F0) {

                    NetworkPrint("Waiting for operation to finish..\n");
                    int count = 0;
                     uint32_t val = 1;
                    do {
                        GSPGPU_ReadHWRegs(stream_element.register_write.physical_address - 0x10100000 + 0x1EC00000 - 0x1EB00000, &val, 4);
                        if (val & 1)
                            break;

                        svcSleepThread(1000);
                    } while(count++ < 100);
                }
                break;
            }
            default:
                NetworkPrint("Unknown stream element type %x", stream_element.type);
                goto exit;
                break;
            }
        }
    }

exit:
    gfxExit();

    NetworkExit();

    return 0;
}
