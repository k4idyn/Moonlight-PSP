/*
 * config.c - Configuration persistence implementation for PSP Moonlight
 *
 * Implements INI-style config file reading/writing using PSP sceIo* functions.
 * Stores configuration on Memory Stick at ms0:/moonlight/config.ini
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "ui_manager.h"
#include "diag_log.h"
#include "settings_menu.h"  /* RESOLUTION_COUNT */

/*--------------------------------------------------------------------------
 * INI Parser Helpers
 *--------------------------------------------------------------------------*/

/* Buffer size for reading config file */
#define CONFIG_BUFFER_SIZE      2048
#define LINE_MAX_LENGTH         256

static ManualHostEntry g_manual_hosts[MAX_MANUAL_HOSTS];
static int g_manual_host_count = 0;
static PspConfig g_last_loaded_config;
static int g_have_last_loaded_config = 0;

/*--------------------------------------------------------------------------
 * Helper: Trim whitespace from string
 *--------------------------------------------------------------------------*/
static char* trimWhitespace(char *str)
{
    char *end;
    
    /* Leading whitespace */
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        str++;
    }
    
    if (*str == '\0') {
        return str;
    }
    
    /* Trailing whitespace */
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        end--;
    }
    
    *(end + 1) = '\0';
    return str;
}

/*--------------------------------------------------------------------------
 * Helper: Parse integer value from INI line
 *--------------------------------------------------------------------------*/
static int parseIntValue(const char *line, int *value)
{
    const char *equals = strchr(line, '=');
    if (!equals) {
        return -1;
    }
    
    equals++;  /* Skip '=' */
    char *trimmed = trimWhitespace((char*)equals);
    
    *value = atoi(trimmed);
    return 0;
}

/*--------------------------------------------------------------------------
 * Helper: Parse string value from INI line
 *--------------------------------------------------------------------------*/
static int parseStringValue(const char *line, char *value, int maxLen)
{
    const char *equals = strchr(line, '=');
    if (!equals) {
        return -1;
    }
    
    equals++;  /* Skip '=' */
    char *trimmed = trimWhitespace((char*)equals);
    
    strncpy(value, trimmed, maxLen - 1);
    value[maxLen - 1] = '\0';
    
    return 0;
}

/*--------------------------------------------------------------------------
 * Helper: Write string to file descriptor
 *--------------------------------------------------------------------------*/
static int writeString(int fd, const char *str)
{
    int len = strlen(str);
    return sceIoWrite(fd, str, len);
}

static void clearManualHosts(void)
{
    memset(g_manual_hosts, 0, sizeof(g_manual_hosts));
    g_manual_host_count = 0;
}

static int findManualHostIndex(const char *ip)
{
    int index;

    if (!ip || !ip[0]) {
        return -1;
    }

    for (index = 0; index < g_manual_host_count; index++) {
        if (strcmp(g_manual_hosts[index].ip, ip) == 0) {
            return index;
        }
    }

    return -1;
}

static void rememberLoadedConfig(const PspConfig *config)
{
    if (!config) {
        return;
    }

    g_last_loaded_config = *config;
    g_have_last_loaded_config = 1;
}

/*--------------------------------------------------------------------------
 * configSetDefaults - Initialize config with default values
 *--------------------------------------------------------------------------*/
void configSetDefaults(PspConfig *config)
{
    /* Stream settings */
    config->width = DEFAULT_WIDTH;           /* 368 — Quality preset (math optimum) */
    config->height = DEFAULT_HEIGHT;         /* 208 — ~16:9, mod-16 aligned */
    config->fps = DEFAULT_FPS;               /* 30 FPS */
    config->bitrate = DEFAULT_BITRATE;       /* 500 kbps flat (proven safe on 802.11b) */
    config->packetSize = DEFAULT_PACKET_SIZE;
    config->streamingRemotely = 2;           /* STREAM_CFG_AUTO */
    config->audioConfiguration = 0x0000CA02; /* Stereo */
    config->supportedVideoFormats = 0x0001;  /* H.264 */
    config->clientRefreshRateX100 = 6000;    /* 60 Hz */
    config->colorSpace = 0;                  /* Rec. 601 */
    config->colorRange = 0;                  /* Limited */
    config->encryptionFlags = 0xFFFFFFFF;    /* All encryption */
    
    /* Clear AES key/IV */
    memset(config->remoteInputAesKey, 0, 16);
    memset(config->remoteInputAesIv, 0, 16);
    
    /* PSP-specific settings */
    config->controlMode = DEFAULT_CONTROL_MODE;
    config->resolutionIndex = 0;             /* Quality preset: 368x208 */
    config->fpsIndex = 0;                    /* 15 FPS (index 0 in [15,20,30,60]) */

    /* Pairing persistence */
    memset(config->pairedHostIp, 0, sizeof(config->pairedHostIp));

    /* Network bind IP (empty = INADDR_ANY, for real hardware) */
    memset(config->localBindIp, 0, sizeof(config->localBindIp));

    /* UI Settings */
    config->uiThemeIndex = 0; /* Classic Blue */
}

/*--------------------------------------------------------------------------
 * loadConfig - Load configuration from config.ini
 *--------------------------------------------------------------------------*/
int loadConfig(PspConfig *config)
{
    int fd;
    char buffer[CONFIG_BUFFER_SIZE];
    char line[LINE_MAX_LENGTH];
    int bytesRead;
    int linePos = 0;
    int fileLoaded = 0;
    
    /* Initialize with defaults first */
    configSetDefaults(config);
    clearManualHosts();
    
    /* Try to open config file */
    fd = sceIoOpen(CONFIG_FILE_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        /* File doesn't exist - return defaults */
        return -1;
    }
    
    /* Read entire file */
    bytesRead = sceIoRead(fd, buffer, CONFIG_BUFFER_SIZE - 1);
    sceIoClose(fd);
    
    if (bytesRead <= 0) {
        diag_log_write("CONFIG", "File empty or unreadable.\n");
        return -1;
    }
    
    buffer[bytesRead] = '\0';  /* Null terminate */
    
    /* Parse line by line */
    char *bufferPtr = buffer;
    while (*bufferPtr) {
        /* Extract line */
        linePos = 0;
        while (*bufferPtr && *bufferPtr != '\n' && linePos < LINE_MAX_LENGTH - 1) {
            line[linePos++] = *bufferPtr++;
        }
        line[linePos] = '\0';
        
        /* Skip newline */
        if (*bufferPtr == '\n') {
            bufferPtr++;
        }
        
        /* Trim whitespace */
        char *trimmed = trimWhitespace(line);
        
        /* Skip empty lines and comments */
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        
        /* Skip section headers [xxx] */
        if (trimmed[0] == '[') {
            continue;
        }
        
        /* Parse key-value pairs */
        int value;
        
        if (strncmp(trimmed, "width", 5) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->width = value;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "height", 6) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->height = value;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "fpsIndex", 8) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->fpsIndex = value;
                /* Rebuild actual fps from index */
                config->fps = (value == 0) ? 15 : (value == 1) ? 30 : (value == 2) ? 40 : 60;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "fps", 3) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->fps = value;
                /* Update fpsIndex for menu display */
                config->fpsIndex = (value <= 15) ? 0 : (value <= 20) ? 1 : (value <= 30) ? 2 : 3;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "bitrate", 7) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                /* Enforce 802.11b ceiling: max 4000 kbps */
                if (value > MAX_BITRATE) value = MAX_BITRATE;
                if (value < 100)         value = 100;
                config->bitrate = value;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "packetSize", 10) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->packetSize = value;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "controlMode", 11) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->controlMode = (value == 0) ? CONTROL_MODE_XBOX : CONTROL_MODE_BROWSER;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "resolutionIndex", 15) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->resolutionIndex = value;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "uiThemeIndex", 12) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->uiThemeIndex = value;
                ui_apply_theme(config->uiThemeIndex);
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "cabacTestMode", 13) == 0) {
            if (parseIntValue(trimmed, &value) == 0) {
                config->cabacTestMode = (value != 0) ? 1 : 0;
                fileLoaded = 1;
            }
        }
        else if (strncmp(trimmed, "paired_host_ip", 14) == 0) {
            parseStringValue(trimmed, config->pairedHostIp, sizeof(config->pairedHostIp));
            fileLoaded = 1;
            diag_log_write("CONFIG", "Loaded paired host: %s\n", config->pairedHostIp);
        }
        else if (strncmp(trimmed, "local_bind_ip", 13) == 0) {
            parseStringValue(trimmed, config->localBindIp, sizeof(config->localBindIp));
            fileLoaded = 1;
        }
        else if (strncmp(trimmed, "manual_host_", 12) == 0) {
            char value_buf[64];
            if (parseStringValue(trimmed, value_buf, sizeof(value_buf)) == 0) {
                char *comma = strchr(value_buf, ',');
                if (g_manual_host_count < MAX_MANUAL_HOSTS) {
                    ManualHostEntry *entry = &g_manual_hosts[g_manual_host_count];
                    memset(entry, 0, sizeof(*entry));
                    if (comma) {
                        *comma = '\0';
                        comma++;
                        strncpy(entry->mac, trimWhitespace(comma), sizeof(entry->mac) - 1);
                    }
                    strncpy(entry->ip, trimWhitespace(value_buf), sizeof(entry->ip) - 1);
                    if (entry->ip[0] != '\0') {
                        g_manual_host_count++;
                        fileLoaded = 1;
                    }
                }
            }
        }
    }
    
    /* Clamp resolutionIndex to valid range */
    if (config->resolutionIndex < 0 || config->resolutionIndex >= RESOLUTION_COUNT)
        config->resolutionIndex = 0;
    
    rememberLoadedConfig(config);

    return (fileLoaded > 0) ? 0 : -1;
}

/*--------------------------------------------------------------------------
 * saveConfig - Save configuration to config.ini
 *--------------------------------------------------------------------------*/
int saveConfig(const PspConfig *config)
{
    int fd;
    char line[LINE_MAX_LENGTH];
    int index;
    diag_log_write("CONFIG", "Saving config - Theme Index: %d\n", config->uiThemeIndex);
    
    /* Create directory if it doesn't exist */
    sceIoMkdir(CONFIG_DIR_PATH, 0777);
    
    /* Open file for writing (create if doesn't exist, truncate if exists) */
    fd = sceIoOpen(CONFIG_FILE_PATH, 
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 
                   0777);
    
    if (fd < 0) {
        return -1;
    }
    
    /* Write header comment */
    writeString(fd, "; Moonlight PSP Configuration File\n");
    writeString(fd, "; This file is auto-generated. Edit with care.\n\n");
    
    writeString(fd, "[stream]\n");
    
    /* Write resolution */
    sprintf(line, "width = %d\n", config->width);
    writeString(fd, line);
    
    sprintf(line, "height = %d\n", config->height);
    writeString(fd, line);
    
    sprintf(line, "resolutionIndex = %d\n", config->resolutionIndex);
    writeString(fd, line);
    
    /* Write FPS */
    sprintf(line, "fps = %d\n", config->fps);
    writeString(fd, line);
    
    sprintf(line, "fpsIndex = %d\n", config->fpsIndex);
    writeString(fd, line);
    
    /* Write bitrate */
    sprintf(line, "bitrate = %d\n", config->bitrate);
    writeString(fd, line);
    
    /* Write packet size */
    sprintf(line, "packetSize = %d\n", config->packetSize);
    writeString(fd, line);
    
    writeString(fd, "\n[controls]\n");
    
    /* Write control mode */
    sprintf(line, "controlMode = %d\n", (int)config->controlMode);
    writeString(fd, line);

    writeString(fd, "\n[hosts]\n");
    for (index = 0; index < g_manual_host_count; index++) {
        sprintf(line, "manual_host_%d = %s,%s\n",
                index,
                g_manual_hosts[index].ip,
                g_manual_hosts[index].mac);
        writeString(fd, line);
    }

    writeString(fd, "\n[pairing]\n");
    sprintf(line, "paired_host_ip = %s\n", config->pairedHostIp);
    writeString(fd, line);

    writeString(fd, "\n[network]\n");
    writeString(fd, "; local_bind_ip: leave empty for real PSP hardware (uses INADDR_ANY).\n");
    writeString(fd, "; For PPSSPP on same host as server, set to a secondary NIC IP alias\n");
    writeString(fd, "; e.g.  local_bind_ip = 10.0.0.100\n");
    sprintf(line, "local_bind_ip = %s\n", config->localBindIp);
    writeString(fd, line);

    writeString(fd, "\n[ui_colors]\n");
    sprintf(line, "uiThemeIndex = %d\n", config->uiThemeIndex);
    writeString(fd, line);

    writeString(fd, "\n");
    
    /* Close file */
    sceIoClose(fd);
    
    rememberLoadedConfig(config);

    return 0;
}

int config_get_manual_host_count(void)
{
    return g_manual_host_count;
}

int config_get_manual_host(int index, ManualHostEntry *out_entry)
{
    if (!out_entry || index < 0 || index >= g_manual_host_count) {
        return -1;
    }

    *out_entry = g_manual_hosts[index];
    return 0;
}

int config_add_manual_host(const char *ip, const char *mac)
{
    int index;
    PspConfig config_to_save;

    if (!ip || !ip[0]) {
        return -1;
    }

    index = findManualHostIndex(ip);
    if (index < 0) {
        if (g_manual_host_count >= MAX_MANUAL_HOSTS) {
            return -1;
        }
        index = g_manual_host_count;
        memset(&g_manual_hosts[index], 0, sizeof(g_manual_hosts[index]));
        strncpy(g_manual_hosts[index].ip, ip, sizeof(g_manual_hosts[index].ip) - 1);
        g_manual_host_count++;
    }

    if (mac && mac[0]) {
        strncpy(g_manual_hosts[index].mac, mac, sizeof(g_manual_hosts[index].mac) - 1);
        g_manual_hosts[index].mac[sizeof(g_manual_hosts[index].mac) - 1] = '\0';
    }

    if (g_have_last_loaded_config) {
        config_to_save = g_last_loaded_config;
    } else {
        configSetDefaults(&config_to_save);
    }

    return saveConfig(&config_to_save);
}

int config_delete_manual_host(const char *ip)
{
    int index;
    int i;
    PspConfig config_to_save;

    if (!ip || !ip[0]) {
        return -1;
    }

    index = findManualHostIndex(ip);
    if (index < 0) {
        return -1;  /* Not found */
    }

    /* Shift remaining entries down */
    for (i = index; i < g_manual_host_count - 1; i++) {
        g_manual_hosts[i] = g_manual_hosts[i + 1];
    }
    memset(&g_manual_hosts[g_manual_host_count - 1], 0, sizeof(ManualHostEntry));
    g_manual_host_count--;

    if (g_have_last_loaded_config) {
        config_to_save = g_last_loaded_config;
    } else {
        configSetDefaults(&config_to_save);
    }

    return saveConfig(&config_to_save);
}