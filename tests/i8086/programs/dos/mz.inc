%macro MZ_HEADER 1
    dw 0x5a4d
    dw (file_end - $$) % 512, (file_end - $$ + 511) / 512
    dw %1, (header_end - $$) / 16
    dw (memory_end - payload_end) / 16, (memory_end - payload_end) / 16
    dw (data_start - header_end) / 16, stack_top - data_start
    dw 0, entry - header_end, 0, 28, 0
%endmacro
%macro MZ_RELOC 1
    dw (%1 - header_end) % 16, (%1 - header_end) / 16
%endmacro
