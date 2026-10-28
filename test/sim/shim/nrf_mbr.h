#pragma once
#include <cstdint>
#define SD_MBR_COMMAND_COPY_BL 1
typedef struct { uint32_t command; union { struct { uint32_t* bl_src; uint32_t bl_len; } copy_bl; } params; } sd_mbr_command_t;
uint32_t sd_mbr_command(sd_mbr_command_t* c);     // sim: copies the bootloader in the node's flash
