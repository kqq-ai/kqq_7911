









EXTERN(
_start




sdfile_vfs_ops
nor_sdfile_vfs_ops
clock_timer1
clock_port
);







BOOT_INFO_SIZE = 52;




TLB_SIZE =0;


SDRAM_SIZE = (0 * 1024 * 1024);
RAM0_SIZE = 0x1c7fe00 - 0x1c00000 - TLB_SIZE - BOOT_INFO_SIZE - 128;
UPDATA_BEG = 0x1c7fe00 - 128;

FREE_DACHE_WAY = 7;
FREE_IACHE_WAY = 0;
CACHE_RAM_SIZE = FREE_DACHE_WAY*4K+FREE_IACHE_WAY*4K;

MEMORY
{
    rom(rx) : ORIGIN = 0x2000120, LENGTH = (4 * 1024 * 1024)
    sdram(rwx) : ORIGIN = 0x4000120, LENGTH = SDRAM_SIZE
    ram0(rwx) : ORIGIN = 0x1c00000 + TLB_SIZE, LENGTH = RAM0_SIZE
    boot_info(rwx) : ORIGIN = 0x1c00000 + TLB_SIZE + RAM0_SIZE, LENGTH = BOOT_INFO_SIZE
    cache_ram(rw) : ORIGIN = 0x1f20000+((8-FREE_IACHE_WAY)*4K), LENGTH = CACHE_RAM_SIZE
}

SECTIONS
{

    . = ORIGIN(rom);
    .text ALIGN(4):
    {
        _text_rodata_begin = .;
        PROVIDE(text_rodata_begin = .);

        *startup.S.o(.text)
        *(.boot_code)
        *(.text*)
        *(.LOG_TAG_CONST*)
        *(.rodata*)
        *(.fat_data_code)

        *(.dlmalloc_code)
        *(.dlmalloc_const)
        *(.mem_heap_code)
        *(.mem_heap_const)

        _os_begin = .;
        PROVIDE(os_begin = .);
        *(.os_code)
        *(.os_const)
        *(.os_str)
        *(.os_critical_code)
        *(.os_port_code)
        *(.os_port_const)
        _os_end = .;
        PROVIDE(os_end = .);

        . = ALIGN(4);
        __VERSION_BEGIN = .;
        KEEP(*(.sys.version))
         __VERSION_END = .;
        *(.jlsp_code)
        . = ALIGN(4);
        *(.jlsp_const)







        . = ALIGN(4);



        btctler_code_start = .;

        BTCTLER_CONTROLLER_CODE_START = .;
  *(.bt_rf_const)
  *(.bt_rf_code)
  *(.vendor_manager_const)
  *(.vendor_manager_code)
  *(.device_manager_const)
  *(.device_manager_code)
  *(.hci_controller_const)
  *(.hci_controller_code)
  *(.hci_interface_const)
  *(.hci_interface_code)
        BTCTLER_CONTROLLER_CODE_SIZE = ABSOLUTE(. - BTCTLER_CONTROLLER_CODE_START);

        BTCTLER_LE_CONTROLLER_CODE_START = .;
        *(.ble_rf_const)
        *(.ble_rf_code)

     *(.ble_ll_const)
     *(.ble_ll_code)
  *(.ble_hci_const)
  *(.ble_hci_code)
        BTCTLER_LE_CONTROLLER_CODE_SIZE = ABSOLUTE(. - BTCTLER_LE_CONTROLLER_CODE_START);

        BTCTLER_CL_CODE_START = .;
        *(.bredr_irq)
        *(.bredr_irq_code)
        *(.bredr_irq_const)

        *(.classic_lmp_const)
        *(.classic_lmp_linkbulk_const)
        *(.classic_lmp_code)
        *(.classic_lmp_linkbulk_code)
  *(.classic_hci_const)
  *(.classic_hci_code)

        LMP_ENC_CODE_START = .;
        *(.classic_lmp_auth_const)
        *(.classic_lmp_bigint_const)
        *(.classic_lmp_crypt_const)
        *(.classic_lmp_ecdh_const)
        *(.classic_lmp_hmac_const)
        *(.classic_lmp_auth_code)
        *(.classic_lmp_bigint_code)
        *(.classic_lmp_crypt_code)
        *(.classic_lmp_ecdh_code)
        *(.classic_lmp_hmac_code)
        LMP_ENC_CODE_SIZE = ABSOLUTE(. - LMP_ENC_CODE_START);

        *(.classic_rf_const)
        *(.classic_rf_code)
        *(.classic_bb_const)
        *(.classic_bb_code)
        BTCTLER_CL_CODE_SIZE = ABSOLUTE(. - BTCTLER_CL_CODE_START);

  . = ALIGN(4);
        *(.classic_tws_const)
        *(.classic_tws_code)
        *(.classic_tws_code.esco)

        *(.tws_media_sync_code)
        *(.tws_media_sync_const)

        *(.tws_data_forward_code)
        *(.tws_data_forward_const)

  . = ALIGN(4);
        tws_func_stub_begin = .;
        KEEP(*(.tws_func_stub))
        tws_func_stub_end = .;

        *(.lmp_irq_code)
        *(.link_bulk_code)
        *(.frame_irq_code)

  . = ALIGN(4);
        *(.link_task_const)
        *(.link_task_code)

  . = ALIGN(4);
        *(.classic_irq_const)
        *(.classic_irq_code)

  . = ALIGN(4);
        *(.tws_irq_code)

  . = ALIGN(4);
        tws_sync_call_begin = .;
        KEEP(*(.tws_sync_call))
        tws_sync_call_end = .;


  . = ALIGN(4);
        tws_sync_channel_begin = .;
        KEEP(*(.tws_sync_channel.0))
        KEEP(*(.tws_sync_channel.1))
        KEEP(*(.tws_sync_channel.2))
        KEEP(*(.tws_sync_channel.3))
        KEEP(*(.tws_sync_channel.4))
        KEEP(*(.tws_sync_channel.5))
        KEEP(*(.tws_sync_channel.6))
        KEEP(*(.tws_sync_channel.7))
        KEEP(*(.tws_sync_channel.8))
        KEEP(*(.tws_sync_channel.9))
        KEEP(*(.tws_sync_channel.10))
        KEEP(*(.tws_sync_channel.11))
        KEEP(*(.tws_sync_channel.12))
        KEEP(*(.tws_sync_channel.13))
        tws_sync_channel_end = .;

        btctler_code_end = .;

  . = ALIGN(4);
 . = ALIGN(4);



        btstack_code_start = .;

        . = ALIGN(4);

        a2dp_source_media_codec_begin = .;
        KEEP(*(.a2dp_source_media_codec))
            a2dp_source_media_codec_end = .;
        a2dp_sink_media_probe_begin = .;
        KEEP(*(.a2dp_sink_media_probe))
            a2dp_sink_media_probe_end = .;

        a2dp_sink_media_codec_begin = .;
        KEEP(*(.a2dp_sink_media_codec))
            a2dp_sink_media_codec_end = .;

        a2dp_event_handler_begin = .;
        KEEP(*(.a2dp_event_handler))
            a2dp_event_handler_end = .;

        sdp_record_item_begin = .;
        KEEP(*(.sdp_record_item))
            sdp_record_item_end = .;

        bt_sleep_begin = .;
        KEEP(*(.bt_sleep))
            bt_sleep_end = .;

  *(.bt_stack_const)
  *(.bt_stack_code)
        *(.ble_db_const)
        *(.ble_db_code)
        *(.ble_sm_const)
        *(.ble_sm_code)
        *(.ble_att_const)
        *(.ble_att_code)
        *(.ble_gatt_const)
        *(.ble_gatt_code)


        BTSTACK_LE_HOST_MESH_CODE_START = .;
        *(.ble_mesh_code)
        *(.ble_mesh_tinycrypt_code)

        *(.ble_mesh_const)
        *(.ble_mesh_tinycrypt_const)
        BTSTACK_LE_HOST_MESH_CODE_SIZE = ABSOLUTE(. - BTSTACK_LE_HOST_MESH_CODE_START);

        btstack_code_end = .;
  . = ALIGN(4);


        BTSTACK_CODE_TOTAL_SIZE = btstack_code_end - btstack_code_start;


        . = ALIGN(4);
        *(.sbc_eng_code)

        . = ALIGN(4);
        *(.h4_const)

        . = ALIGN(4);
        crypto_begin = .;
        *(.crypto_ecdh_code)
        *(.crypto_ecdh_const)

        *(.crypto_bigint_code)
        *(.crypto_bigint_const)

        *(.crypto_code)
        *(.crypto_const)

        *(.ECDH_sample_code)
        *(.ECDH_sample_const)

        *(.uECC_code)
        *(.uECC_const)

        *(.hmac_code)
        *(.hmac_const)

        *(.hash_sample_code)
        *(.hash_sample_const)

        *(.aes_cmac_sample_code)
        *(.aes_cmac_sample_const)
        crypto_end = .;
        crypto_size = . - crypto_begin;


        . = ALIGN(4);
        *(.update_const)
        *(.update_code)
        *(.bt_updata_ram_code)






        . = ALIGN(4);


    _vfs_ops_begin = .;
    PROVIDE(vfs_ops_begin = .);
        *(.vfs_operations)
    _vfs_ops_end = .;
    PROVIDE(vfs_ops_end = .);

    _initcall_begin = .;
    PROVIDE(initcall_begin = .);
        *(.initcall)
    _initcall_end = .;
    PROVIDE(initcall_end = .);

    _early_initcall_begin = .;
    PROVIDE(early_initcall_begin = .);
       *(.early.initcall)
    _early_initcall_end = .;
    PROVIDE(early_initcall_end = .);

    _late_initcall_begin = .;
    PROVIDE(late_initcall_begin = .);
       *(.late.initcall)
    _late_initcall_end = .;
    PROVIDE(late_initcall_end = .);

    _platform_initcall_begin = .;
    PROVIDE(platform_initcall_begin = .);
       *(.platform.initcall)
    _platform_initcall_end = .;
    PROVIDE(platform_initcall_end = .);

    _module_initcall_begin = .;
    PROVIDE(module_initcall_begin = .);
       *(.module.initcall)
    _module_initcall_end = .;
    PROVIDE(module_initcall_end = .);

    _sys_event_handler_begin = .;
    PROVIDE(sys_event_handler_begin = .);
       *(.sys_event.*)
    _sys_event_handler_end = .;
    PROVIDE(sys_event_handler_end = .);

    _syscfg_ops_begin = .;
    PROVIDE(syscfg_ops_begin = .);
       *(.syscfg.2.ops)
       *(.syscfg.1.ops)
       *(.syscfg.0.ops)
    _syscfg_ops_end = .;
    PROVIDE(syscfg_ops_end = .);

    _server_info_begin = .;
    PROVIDE(server_info_begin = .);
       *(.server_info)
    _server_info_end = .;
    PROVIDE(server_info_end = .);

    _bus_device_begin = .;
    PROVIDE(bus_device_begin = .);
       *(.bus_device)
    _bus_device_end = .;
    PROVIDE(bus_device_end = .);

    _sys_power_hal_ops_begin = .;
    PROVIDE(sys_power_hal_ops_begin = .);
       *(.sys_power_hal_ops)
    _sys_power_hal_ops_end = .;
    PROVIDE(sys_power_hal_ops_end = .);

    _device_node_begin = .;
    PROVIDE(device_node_begin = .);
        *(.device)
    _device_node_end = .;
    PROVIDE(device_node_end = .);

    lp_target_begin = .;
    PROVIDE(lp_target_begin = .);
        *(.lp_target)
    lp_target_end = .;
    PROVIDE(lp_target_end = .);

    _ai_sdk_api_begin = .;
    PROVIDE(ai_sdk_api_begin = .);
        *(.ai_sdk)
    _ai_sdk_api_end = .;
    PROVIDE(ai_sdk_api_end = .);

    config_target_begin = .;
    PROVIDE(config_target_begin = .);
        *(.config_target)
    config_target_end = .;
    PROVIDE(config_target_end = .);

    _lcd_device_begin = .;
    PROVIDE(lcd_device_begin = .);
        *(.lcd)
    _lcd_device_end = .;
    PROVIDE(lcd_device_end = .);

    _camera_dev_begin = .;
    PROVIDE(camera_dev_begin = .);
        *(.camera_dev)
    _camera_dev_end = .;
    PROVIDE(camera_dev_end = .);

    _camera1_dev_begin = .;
    PROVIDE(camera1_dev_begin = .);
        *(.camera1_dev)
    _camera1_dev_end = .;
    PROVIDE(camera1_dev_end = .);

    _iic_device_begin = .;
    PROVIDE(iic_device_begin = .);
        *(.iic)
    _iic_device_end = .;
    PROVIDE(iic_device_end = .);

    _video_pkg_sys_begin = .;
    PROVIDE(video_pkg_sys_begin = .);
        *(.video_pkg_sys)
    _video_pkg_sys_end = .;
    PROVIDE(video_pkg_sys_end = .);

    _video_pkg_begin = .;
    PROVIDE(video_pkg_begin = .);
        *(.video_pkg)
    _video_pkg_end = .;
    PROVIDE(video_pkg_end = .);
 . = ALIGN(4);

        lcd_interface_begin = .;
        KEEP(*(.lcd_if_info))
        lcd_interface_end = .;

  ui_style_begin = .;
  KEEP(*(.ui_style))
  ui_style_end = .;


        elm_event_handler_begin_JL = .;
        KEEP(*(.elm_event_handler_JL))
  elm_event_handler_end_JL = .;

        elm_event_handler_begin_UPGRADE = .;
        KEEP(*(.elm_event_handler_UPGRADE))
        elm_event_handler_end_UPGRADE = .;



  elm_event_handler_begin_DIAL = .;
        KEEP(*(.elm_event_handler_DIAL))
  elm_event_handler_end_DIAL = .;


  control_event_handler_begin = .;
        KEEP(*(.control_event_handler))
  control_event_handler_end = .;

        control_ops_begin = .;
        KEEP(*(.control_ops))
        control_ops_end = .;

        battery_notify_begin = .;
        *(.battery_notify)
        battery_notify_end = .;
 . = ALIGN(4);


   _audio_decoder_begin = .;
   PROVIDE(audio_decoder_begin = .);
       *(.audio_decoder)
   _audio_decoder_end = .;
   PROVIDE(audio_decoder_end = .);

   _audio_package_begin = .;
   PROVIDE(audio_package_begin = .);
       *(.audio_package)
   _audio_package_end = .;
   PROVIDE(audio_package_end = .);

   _audio_effect_begin = .;
   PROVIDE(audio_effect_begin = .);
       *(.audio_effect)
   _audio_effect_end = .;
   PROVIDE(audio_effect_end = .);

   _audio_hwaccel_begin = .;
   PROVIDE(audio_hwaccel_begin = .);
       *(.audio_hwaccel)
   _audio_hwaccel_end = .;
   PROVIDE(audio_hwaccel_end = .);

   _audio_output_handler_begin = .;
   PROVIDE(audio_output_handler_begin = .);
       *(.audio_output_handler)
   _audio_output_handler_end = .;
   PROVIDE(audio_output_handler_end = .);

   _audio_input_handler_begin = .;
   PROVIDE(audio_input_handler_begin = .);
       *(.audio_input_handler)
   _audio_input_handler_end = .;
   PROVIDE(audio_input_handler_end = .);

   _audio_subdev_begin = .;
   PROVIDE(audio_subdev_begin = .);
       *(.audio_subdev.0)
       *(.audio_subdev.1)
       *(.audio_subdev.2)
       *(.audio_subdev.3)
   _audio_subdev_end = .;
   PROVIDE(audio_subdev_end = .);

   _fm_dev_begin = .;
   PROVIDE(fm_dev_begin = .);
       *(.fm_dev)
   _fm_dev_end = .;
   PROVIDE(fm_dev_end = .);

    _video_dev_begin = .;
    PROVIDE(video_dev_begin = .);
        *(.video_device)
    _video_dev_end = .;
    PROVIDE(video_dev_end = .);

    _video_subdev_begin = .;
    PROVIDE(video_subdev_begin = .);
        *(.video_subdev.0)
        *(.video_subdev.1)
        *(.video_subdev.2)
        *(.video_subdev.3)
        *(.video_subdev.4)
        *(.video_subdev.5)
    _video_subdev_end = .;
    PROVIDE(video_subdev_end = .);

        PROVIDE(__fsymtab_start = .);
        KEEP(*(.FSymTab))
        PROVIDE(__fsymtab_end = .);
        PROVIDE(__vsymtab_start = .);
        KEEP(*(.VSymTab))
        PROVIDE(__vsymtab_end = .);
        PROVIDE(__rtatcmdtab_start = .);
        KEEP(*(.RtAtCmdTab))
        PROVIDE(__rtatcmdtab_end = .);

        . = ALIGN(32);
        _text_rodata_end = .;
        PROVIDE(text_rodata_end = .);
    } >rom





    . = ORIGIN(sdram);
    .data ALIGN(32):
    {
        . = ALIGN(4);
    } > sdram

    .bss ALIGN(32):
    {
        . = ALIGN(4);
    } > sdram


    . =ORIGIN(ram0);
    .ram0_data ALIGN(4):
    {
        _ram_text_rodata_begin = .;
        PROVIDE(ram_text_rodata_begin = .);
        _VM_CODE_START = . ;
        *(.vm)
        _VM_CODE_END = . ;
        . = ALIGN(4);
        _SPI_CODE_START = . ;
        *(.spi_code)
        . = ALIGN(4);
        _SPI_CODE_END = . ;
        *(.flushinv_icache)
        *(.volatile_ram_code)

        _ram_text_rodata_end = .;
        PROVIDE(ram_text_rodata_end = .);

        . = ALIGN(4);
        *(.data)

        . = ALIGN(4);
        *(.volatile_ram)
        *(.non_volatile_ram)
        *(.fft_data)
        *(.deepsleep_target)
        *(.os_data)
        *(.os_port_data)





        . = ALIGN(4);


  _ctp_mapping_tab_begin = .;
 PROVIDE(ctp_mapping_tab_begin = .);

 _ctp_mapping_tab_system_cmd_begin = .;
 PROVIDE(ctp_mapping_tab_system_cmd_begin = .);
  *(.ctp_system_cmd)
 _ctp_mapping_tab_system_cmd_end = .;
 PROVIDE(ctp_mapping_tab_system_cmd_end = .);

 _ctp_mapping_tab_video_cmd_begin = .;
 PROVIDE(ctp_mapping_tab_video_cmd_begin = .);
  *(.ctp_video_cmd)
 _ctp_mapping_tab_video_cmd_end = .;
 PROVIDE(ctp_mapping_tab_video_cmd_end = .);

 _ctp_mapping_tab_photo_cmd_begin = .;
 PROVIDE(ctp_mapping_tab_photo_cmd_begin = .);
  *(.ctp_photo_cmd)
 _ctp_mapping_tab_photo_cmd_end = .;
 PROVIDE(ctp_mapping_tab_photo_cmd_end = .);

 _ctp_mapping_tab_end = .;
 PROVIDE(ctp_mapping_tab_end = .);

 _net_video_stream_sub_begin = .;
 PROVIDE(net_video_stream_sub_begin = .);
   *(.net_video_stream)
 _net_video_stream_sub_end = .;
 PROVIDE(net_video_stream_sub_end = .);
 . = ALIGN(4);


        btctler_data_start = .;

        BTCTLER_CONTROLLER_DATA_START = .;
        *(.bt_rf_data)
  *(.vendor_manager_data)
  *(.device_manager_data)
  *(.hci_controller_data)
  *(.hci_interface_data)
        BTCTLER_CONTROLLER_DATA_SIZE = ABSOLUTE(. - BTCTLER_CONTROLLER_DATA_START);

        BTCTLER_LE_CONTROLLER_DATA_START = .;
        *(.ble_ll_data)
        *(.ble_hci_data)
        *(.classic_hci_data)
        *(.ble_rf_data)
        BTCTLER_LE_CONTROLLER_DATA_SIZE = ABSOLUTE(. - BTCTLER_LE_CONTROLLER_DATA_START);

        BTCTLER_CL_DATA_START = .;
        *(.classic_lmp_data)
        *(.classic_lmp_auth_data)
        *(.classic_lmp_bigint_data)
        *(.classic_lmp_crypt_data)
        *(.classic_lmp_ecdh_data)
        *(.classic_lmp_linkbulk_data)
        *(.classic_lmp_hmac_data)
        *(.classic_rf_data)
        *(.classic_bb_data)
        BTCTLER_CL_DATA_SIZE = ABSOLUTE(. - BTCTLER_CL_DATA_START);

        btctler_data_end = .;
 . = ALIGN(4);


        btstack_data_start = .;
        *(.bt_stack_data)
        *(.ble_db_data)
        *(.ble_sm_data)
        *(.ble_att_data)
        *(.ble_gatt_data)


        BTSTACK_LE_HOST_MESH_DATA_START = .;
  . = (. +3) & ~ 3;
        _net_buf_pool_list = .;
        *(._net_buf_pool.static.*)

        *(.ble_mesh_data)
        *(.ble_mesh_tinycrypt_data)
        BTSTACK_LE_HOST_MESH_DATA_SIZE = ABSOLUTE(. - BTSTACK_LE_HOST_MESH_DATA_START);


        btstack_data_end = .;

        . = ALIGN(4);
    } > ram0

    .ram0_bss ALIGN(4) (NOLOAD):
    {
        . = ALIGN(4);


        btctler_bss_start = .;

        BTCTLER_CONTROLLER_BSS_START = .;

        *(.bd_base)
        *(.bredr_rxtx_bulk)
        acl_tx_pool = .;
        *(.bredr_tx_bulk)

        acl_tx_pool_end = acl_tx_pool + 0;



        . = acl_tx_pool_end;

        acl_rx_pool = .;
        *(.bredr_rx_bulk)

        acl_rx_pool_end = acl_rx_pool + 0;



        . = acl_rx_pool_end;

        tws_bulk_pool = .;



        tws_bulk_pool_end = tws_bulk_pool;

        . = tws_bulk_pool_end;

        *(.bt_rf_bss)
  *(.hci_controller_bss)
  *(.hci_interface_bss)
  *(.device_manager_bss)
  *(.vendor_manager_bss)
        BTCTLER_CONTROLLER_BSS_SIZE = ABSOLUTE(. - BTCTLER_CONTROLLER_BSS_START);

        BTCTLER_LE_CONTROLLER_BSS_START = .;
        *(.ble_hci_bss)
        *(.ble_ll_bss)
        *(.ble_rf_bss)
        BTCTLER_LE_CONTROLLER_BSS_SIZE = ABSOLUTE(. - BTCTLER_LE_CONTROLLER_BSS_START);

        BTCTLER_CL_BSS_START = .;
        *(.classic_rf_bss)
        *(.classic_lmp_bss)
        *(.classic_lmp_auth_bss)
        *(.classic_lmp_bigint_bss)
        *(.classic_lmp_crypt_bss)
        *(.classic_lmp_ecdh_bss)
        *(.classic_lmp_linkbulk_bss)
        *(.classic_lmp_hmac_bss)
        *(.classic_bb_bss)
        *(.classic_hci_bss)
        BTCTLER_CL_BSS_SIZE = ABSOLUTE(. - BTCTLER_CL_BSS_START);

        btctler_bss_end = .;
 . = ALIGN(4);


        btstack_bss_start = .;
        *(.bt_stack_bss)
        *(.ble_db_bss)
        *(.ble_sm_bss)
        *(.ble_att_bss)
        *(.ble_gatt_bss)
        *(.btstack_pool)


        BTSTACK_LE_HOST_MESH_BSS_START = .;
        *(.ble_mesh_bss)
        *(.ble_mesh_tinycrypt_bss)
        BTSTACK_LE_HOST_MESH_BSS_SIZE = ABSOLUTE(. - BTSTACK_LE_HOST_MESH_BSS_START);

        btstack_bss_end = .;

        *(.bss)



        *(COMMON)
        *(.mem_heap)

        . = ALIGN(4);
        *(.the_debug_isr_stack_c0)
        *(.the_debug_isr_stack_c1)

        _stack_info_begin = .;
        PROVIDE(stack_info_begin = .);

        *(.stack)
        *(.stack_magic)
        *(.stack_magic0)

        *(.cpu0_ustack)
        _cpu0_sstack_begin = .;
        PROVIDE(cpu0_sstack_begin = .);
        *(.cpu0_sstack)
        _cpu0_sstack_end = .;
        PROVIDE(cpu0_sstack_end = .);

        *(.cpu1_ustack)
        _cpu1_sstack_begin = .;
        PROVIDE(cpu1_sstack_begin = .);
        *(.cpu1_sstack)
        _cpu1_sstack_end = .;
        PROVIDE(cpu1_sstack_end = .);

        _stack_info_end = .;
        PROVIDE(stack_info_end = .);




        *(.sd_var)
        *(.sram)
        . = ALIGN(4);
        *(.os_port_bss)
        *(.os_bss)
        *(.update_bss)
        . = ALIGN(4);


_usb_data_begin = .;
PROVIDE(usb_data_begin = .);
*(.usb_h_bss)
*(.usb_g_bss)
*(.usb_h_dma)
*(.usb_config_var)
*(.usb_msd_dma)
*(.usb_fifo)
*(.mass_storage)
*(.uac_var)
*(.uac_rx)
*(.usb_h_udisk)
*(.cdc_var)
_usb_data_end = .;
PROVIDE(usb_data_end = .);

        . = ALIGN(4);
    } > ram0

    _HEAP_BEGIN = ( . + 31 ) / 32 * 32 ;
    PROVIDE(HEAP_BEGIN = (. + 31) / 32 * 32);
    _HEAP_END = 0x1c00000 + RAM0_SIZE - 32;
    PROVIDE(HEAP_END = 0x1c00000 + RAM0_SIZE - 32);
    _MALLOC_SIZE = _HEAP_END - _HEAP_BEGIN;
    PROVIDE(MALLOC_SIZE = _HEAP_END - _HEAP_BEGIN);

    PROVIDE(RAM_HEAP_BEGIN = 0);
    PROVIDE(RAM_HEAP_END = 0);
    PROVIDE(RAM_MALLOC_SIZE = 0);

    . =ORIGIN(boot_info);
    .boot_info ALIGN(4):
    {
  *(.boot_info)
        . = ALIGN(4);
    } > boot_info


    . =ORIGIN(cache_ram);
    .cache_ram_data ALIGN(4):
    {

 _app_begin = .;
 PROVIDE(app_begin = .);
    *(.app)
 _app_end = .;
 PROVIDE(app_end = .);

 _static_hi_timer_begin = .;
 PROVIDE(static_hi_timer_begin = .);
    *(.hi_timer)
 _static_hi_timer_end = .;
 PROVIDE(static_hi_timer_end = .);

 _sys_cpu_timer_begin = .;
 PROVIDE(sys_cpu_timer_begin = .);
    *(.sys_cpu_timer)
 _sys_cpu_timer_end = .;
 PROVIDE(sys_cpu_timer_end = .);

 _gsensor_dev_begin = .;
 PROVIDE(gsensor_dev_begin = .);
     *(.gsensor_dev)
 _gsensor_dev_end = .;
 PROVIDE(gsensor_dev_end = .);

 _clock_critical_handler_begin = .;
 PROVIDE(clock_critical_handler_begin = .);
    *(.clock_critical_txt)
 _clock_critical_handler_end = .;
 PROVIDE(clock_critical_handler_end = .);
 . = ALIGN(4);
    } > cache_ram

    .cache_ram_bss ALIGN(4):
    {
        *(.memp_memory_x)
        . = ALIGN(4);
    } > cache_ram


}

    text_begin = ADDR(.text);
    text_size = SIZEOF(.text);
    PROVIDE(text_size = SIZEOF(.text));
    bss_begin = ADDR(.bss);
    bss_size = SIZEOF(.bss);
    PROVIDE(bss_size = SIZEOF(.bss));

    data_vma = ADDR(.data);
    data_lma = text_begin + SIZEOF(.text);
    data_size = SIZEOF(.data);
    PROVIDE(data_size = SIZEOF(.data));


    _ram0_bss_vma = ADDR(.ram0_bss);
    _ram0_bss_size = SIZEOF(.ram0_bss);
    PROVIDE(ram0_bss_size = SIZEOF(.ram0_bss));

    _ram0_data_vma = ADDR(.ram0_data);
    _ram0_data_lma = text_begin + SIZEOF(.text) + SIZEOF(.data);
    _ram0_data_size = SIZEOF(.ram0_data);
    PROVIDE(ram0_data_size = SIZEOF(.ram0_data));


    PROVIDE(cache_ram_bss_vma = ADDR(.cache_ram_bss));
    PROVIDE(cache_ram_bss_size = SIZEOF(.cache_ram_bss));
    PROVIDE(cache_ram_data_vma = ADDR(.cache_ram_data));
    PROVIDE(cache_ram_data_lma = text_begin + SIZEOF(.text) + SIZEOF(.data) + SIZEOF(.ram0_data));
    PROVIDE(cache_ram_data_size = SIZEOF(.cache_ram_data));



    BTCTLER_LE_RAM_TOTAL = BTCTLER_LE_CONTROLLER_DATA_SIZE + BTCTLER_LE_CONTROLLER_BSS_SIZE;
    BTCTLER_LE_CODE_TOTAL = BTCTLER_LE_CONTROLLER_CODE_SIZE;

    BTCTLER_CL_RAM_TOTAL = BTCTLER_CL_DATA_SIZE + BTCTLER_CL_BSS_SIZE;
    BTCTLER_CL_CODE_TOTAL = BTCTLER_CL_CODE_SIZE;

    BTCTLER_COMMON_RAM_TOTAL = BTCTLER_CONTROLLER_BSS_SIZE + BTCTLER_CONTROLLER_DATA_SIZE;
    BTCTLER_COMMON_CODE_TOTAL = BTCTLER_CONTROLLER_CODE_SIZE ;

    BTCTLER_RAM_TOTAL = (btctler_data_end - btctler_data_start) + (btctler_bss_end - btctler_bss_start);
    BTCTLER_CODE_TOTAL = (btctler_code_end - btctler_code_start);

    BTSTACK_LE_HOST_MESH_RAM_TOTAL = BTSTACK_LE_HOST_MESH_DATA_SIZE + BTSTACK_LE_HOST_MESH_BSS_SIZE;
    BTSTACK_LE_HOST_MESH_FLASH_TOTAL = BTSTACK_LE_HOST_MESH_CODE_SIZE;


EXTERN(
    lib_audio_server_version
    lib_system_version
    lib_btstack_version
    lib_btctler_version
    lib_btmesh_version
    lib_cfg_tool_version
    lib_common_version
    lib_crcm_version
    lib_cryto_toolbox_version
    lib_event_version
    lib_finsh_version
    lib_fs_version
    lib_driver_version
    lib_video_dec_version
    lib_video_rec_version
    lib_update_version
    lib_ui_font_version
    lib_wifi_version
    lib_wl80_rf_version
    lib_httpcli_version
    lib_airplay_version
    lib_aliyun_version
    lib_cjson_version
    lib_dcl_version
    lib_wpa_supplicant_version
    lib_device_vad_version
    lib_dlna_version
    lib_dui_version
    lib_echocloud_version
    lib_ftp_client_version
    lib_hsm_version
    lib_http_server_version
    lib_profile_version
    lib_iperf_version
    lib_jl_cloud_version
    lib_json_c_version
    lib_kcp_version
    lib_coap_version
    lib_duer_version
    lib_mpeg_version
    lib_lwip_2_0_3_version
    lib_lwip_2_1_2_version
    lib_lwip_2_1_3_version
    lib_mbedtls_2_2_1_version
    lib_mbedtls_2_26_0_version
    lib_mdns_version
    lib_mongoose_version
    lib_mqtt_version
    lib_network_download_version
    lib_rtp_version
    lib_net_server_version
    lib_media_server_version
    lib_stupid_ftpd_version
    lib_ftpserver_version
    lib_telecom_version
    lib_tencent_version
    lib_tmallgenie_version
    lib_tunnel_version
    lib_turing_version
    lib_uc_httpc_version
    lib_uip_version
    lib_websocket_version
    lib_wolfmqtt_version
    lib_wolfssl_version
    lib_wt_version
    lib_zliblite_version
    app_wifi_story_machine_version
    app_wifi_camera_version
    app_scan_box_version
    lib_font_version
    lib_res_version
    lib_ui_draw_version
    lib_ui_version
)

