// Control identifiers for the Infovox 330 configuration utility.
//
// Every input on every page has a distinct id so that its label can be rewritten at runtime
// with the range the engine actually reports for the selected voice. A label that says
// "Speaking rate in words per minute" is worth much less to someone who cannot see the
// slider than one that says "between 45 and 499".

#pragma once

#define IDD_MAIN                100
#define IDD_PAGE_VOICES         101
#define IDD_PAGE_TEXT           102
#define IDD_PAGE_ENGINE         103
#define IDD_NAME_PROMPT         104

#define IDC_TABS                1000
#define IDC_APPLY               1001
#define IDC_STATUS              1002
#define IDC_STATUS_LABEL        1003

// ---- the Voices page ------------------------------------------------------------------
#define IDC_VOICE_LIST          1100
#define IDC_VOICE_NEW           1101
#define IDC_VOICE_RENAME        1102
#define IDC_VOICE_DELETE        1103
#define IDC_DISPLAY_NAME        1104
#define IDC_BASE_VOICE          1105
#define IDC_BASE_LABEL          1106
#define IDC_RATE                1107
#define IDC_RATE_SPIN           1108
#define IDC_RATE_LABEL          1109
#define IDC_RATE_DEFAULT        1110
#define IDC_PITCH               1111
#define IDC_PITCH_SPIN          1112
#define IDC_PITCH_LABEL         1113
#define IDC_PITCH_DEFAULT       1114
#define IDC_VOLUME              1115
#define IDC_VOLUME_SPIN         1116
#define IDC_PREVIEW_TEXT        1117
#define IDC_PREVIEW_SPEAK       1118
#define IDC_PREVIEW_STOP        1119
#define IDC_CAPABILITIES        1120
#define IDC_DISPLAY_LABEL       1121

// ---- the Text and reporting page -------------------------------------------------------
#define IDC_TEXT_FOR            1200
#define IDC_SUBSTITUTIONS       1201
#define IDC_PREFIX              1202
#define IDC_RATE_SPAN           1203
#define IDC_PITCH_SPAN          1204
#define IDC_LANGUAGE            1205
#define IDC_GENDER              1206
#define IDC_AGE                 1207

// ---- the Speech engine page ------------------------------------------------------------
#define IDC_WORD_EVENTS         1300
#define IDC_SENTENCE_EVENTS     1301
#define IDC_SOFTWARE_VOLUME     1302
#define IDC_HIDE_BUILTIN        1303
#define IDC_SET_ENGINE_VOLUME   1304
#define IDC_REALTIME            1305
#define IDC_REALTIME_ENABLE     1306
#define IDC_LOG_LEVEL           1307
#define IDC_OPEN_LOGS           1308
#define IDC_OPEN_CONFIG         1309
#define IDC_ENGINE_REPORT       1310
#define IDC_CONFIG_PATH         1311
#define IDC_CONTROL_TAGS        1312

// ---- the name prompt -------------------------------------------------------------------
#define IDC_PROMPT_LABEL        1400
#define IDC_PROMPT_EDIT         1401

#ifndef IDC_STATIC
#define IDC_STATIC              (-1)
#endif
