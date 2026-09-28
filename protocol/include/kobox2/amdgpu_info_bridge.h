/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_AMDGPU_INFO_BRIDGE_H
#define KOBOX2_AMDGPU_INFO_BRIDGE_H

/* Linux's C UAPI structures contain alignment holes; the Kobox2 records do
 * not. Keep this field mapping shared by the two ends of the bridge so an
 * INFO reply never exposes Linux padding or silently shifts later fields. */
#define KB2_AMDGPU_HW_IP_FIELDS(X) \
	X(hw_ip_version_major, VERSION_MAJOR) \
	X(hw_ip_version_minor, VERSION_MINOR) \
	X(capabilities_flags, CAPABILITIES) \
	X(ib_start_alignment, IB_START_ALIGNMENT) \
	X(ib_size_alignment, IB_SIZE_ALIGNMENT) \
	X(available_rings, AVAILABLE_RINGS) \
	X(ip_discovery_version, IP_DISCOVERY_VERSION) \
	X(userq_num_slots, USERQ_NUM_SLOTS)

#define KB2_AMDGPU_DEVICE_FIELDS(X) \
	X(device_id, DEVICE_ID) \
	X(chip_rev, CHIP_REV) \
	X(external_rev, EXTERNAL_REV) \
	X(pci_rev, PCI_REV) \
	X(family, FAMILY) \
	X(num_shader_engines, NUM_SHADER_ENGINES) \
	X(num_shader_arrays_per_engine, NUM_SHADER_ARRAYS_PER_ENGINE) \
	X(gpu_counter_freq, GPU_COUNTER_FREQUENCY) \
	X(max_engine_clock, MAX_ENGINE_CLOCK) \
	X(max_memory_clock, MAX_MEMORY_CLOCK) \
	X(cu_active_number, CU_ACTIVE_NUMBER) \
	X(cu_ao_mask, CU_ALWAYS_ON_MASK) \
	X(cu_bitmap, CU_BITMAP) \
	X(enabled_rb_pipes_mask, ENABLED_RB_PIPES_MASK) \
	X(num_rb_pipes, NUM_RB_PIPES) \
	X(num_hw_gfx_contexts, NUM_HW_GFX_CONTEXTS) \
	X(pcie_gen, PCIE_GENERATION) \
	X(ids_flags, IDS_FLAGS) \
	X(virtual_address_offset, VIRTUAL_ADDRESS_OFFSET) \
	X(virtual_address_max, VIRTUAL_ADDRESS_MAX) \
	X(virtual_address_alignment, VIRTUAL_ADDRESS_ALIGNMENT) \
	X(pte_fragment_size, PTE_FRAGMENT_SIZE) \
	X(gart_page_size, GART_PAGE_SIZE) \
	X(ce_ram_size, CE_RAM_SIZE) \
	X(vram_type, VRAM_TYPE) \
	X(vram_bit_width, VRAM_BIT_WIDTH) \
	X(vce_harvest_config, VCE_HARVEST_CONFIG) \
	X(gc_double_offchip_lds_buf, GC_DOUBLE_OFFCHIP_LDS_BUF) \
	X(prim_buf_gpu_addr, PRIM_BUF_GPU_ADDR) \
	X(pos_buf_gpu_addr, POS_BUF_GPU_ADDR) \
	X(cntl_sb_buf_gpu_addr, CONTROL_SIDEBAND_GPU_ADDR) \
	X(param_buf_gpu_addr, PARAMETER_CACHE_GPU_ADDR) \
	X(prim_buf_size, PRIM_BUF_SIZE) \
	X(pos_buf_size, POS_BUF_SIZE) \
	X(cntl_sb_buf_size, CONTROL_SIDEBAND_SIZE) \
	X(param_buf_size, PARAMETER_CACHE_SIZE) \
	X(wave_front_size, WAVEFRONT_SIZE) \
	X(num_shader_visible_vgprs, NUM_SHADER_VISIBLE_VGPRS) \
	X(num_cu_per_sh, NUM_CU_PER_SHADER_ARRAY) \
	X(num_tcc_blocks, NUM_TCC_BLOCKS) \
	X(gs_vgt_table_depth, GS_VGT_TABLE_DEPTH) \
	X(gs_prim_buffer_depth, GS_PRIM_BUFFER_DEPTH) \
	X(max_gs_waves_per_vgt, MAX_GS_WAVES_PER_VGT) \
	X(pcie_num_lanes, PCIE_NUM_LANES) \
	X(cu_ao_bitmap, CU_ALWAYS_ON_BITMAP) \
	X(high_va_offset, HIGH_VA_OFFSET) \
	X(high_va_max, HIGH_VA_MAX) \
	X(pa_sc_tile_steering_override, PA_SC_TILE_STEERING_OVERRIDE) \
	X(tcc_disabled_mask, TCC_DISABLED_MASK) \
	X(min_engine_clock, MIN_ENGINE_CLOCK) \
	X(min_memory_clock, MIN_MEMORY_CLOCK) \
	X(tcp_cache_size, TCP_CACHE_SIZE) \
	X(num_sqc_per_wgp, NUM_SQC_PER_WGP) \
	X(sqc_data_cache_size, SQC_DATA_CACHE_SIZE) \
	X(sqc_inst_cache_size, SQC_INSTRUCTION_CACHE_SIZE) \
	X(gl1c_cache_size, GL1_CACHE_SIZE) \
	X(gl2c_cache_size, GL2_CACHE_SIZE) \
	X(mall_size, MALL_SIZE) \
	X(enabled_rb_pipes_mask_hi, ENABLED_RB_PIPES_MASK_HIGH) \
	X(shadow_size, SHADOW_SIZE) \
	X(shadow_alignment, SHADOW_ALIGNMENT) \
	X(csa_size, CSA_SIZE) \
	X(csa_alignment, CSA_ALIGNMENT) \
	X(userq_ip_mask, USERQ_IP_MASK)

#endif
