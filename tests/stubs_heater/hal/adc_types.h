#pragma once
typedef enum { ADC_UNIT_1 = 0, ADC_UNIT_2 } adc_unit_t;
typedef enum {
    ADC_CHANNEL_0 = 0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3, ADC_CHANNEL_4,
} adc_channel_t;
typedef enum { ADC_ATTEN_DB_0 = 0, ADC_ATTEN_DB_12 = 3 } adc_atten_t;
typedef enum { ADC_BITWIDTH_DEFAULT = 0, ADC_BITWIDTH_12 = 12 } adc_bitwidth_t;
