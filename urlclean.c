/*
 * urlclean - A lightweight URL cleaner to remove tracking parameters
 * 
 * Inspired by "Léon - The URL Cleaner" Android app.
 * Removes common tracking parameters from URLs for privacy.
 *
 * (c) 2024 by gatopeich, licensed under a Creative Commons Attribution 3.0
 * Unported License: http://creativecommons.org/licenses/by/3.0/
 * Briefly: Use it however suits you better and just give me due credit.
 *
 * Usage:
 *   urlclean [URL]           - Clean URL from command line argument
 *   urlclean                  - Clean URL from clipboard (requires xclip)
 *   echo "URL" | urlclean    - Clean URL from stdin
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "1.0"
#endif

// Tracking parameters to remove
// Based on common tracking parameters from various sources
static const char* tracking_params[] = {
    // Google Analytics
    "utm_source",
    "utm_medium",
    "utm_campaign",
    "utm_term",
    "utm_content",
    "utm_id",
    "utm_source_platform",
    "utm_creative_format",
    "utm_marketing_tactic",
    
    // Facebook
    "fbclid",
    "fb_action_ids",
    "fb_action_types",
    "fb_ref",
    "fb_source",
    
    // Twitter/X
    "twclid",
    "s", // Twitter share parameter
    
    // Microsoft/Bing
    "msclkid",
    
    // Google
    "gclid",
    "gclsrc",
    "dclid",
    "gbraid",
    "wbraid",
    
    // Mailchimp
    "mc_cid",
    "mc_eid",
    
    // Hubspot
    "hsa_acc",
    "hsa_cam",
    "hsa_grp",
    "hsa_ad",
    "hsa_src",
    "hsa_tgt",
    "hsa_kw",
    "hsa_mt",
    "hsa_net",
    "hsa_ver",
    
    // Adobe
    "s_kwcid",
    "ef_id",
    
    // Outbrain
    "obOrigUrl",
    
    // Taboola
    "tblci",
    
    // Yahoo
    "yclid",
    
    // Instagram
    "igshid",
    
    // TikTok
    "ttclid",
    "_ttp",
    
    // Pinterest
    "epik",
    
    // Marketo
    "mkt_tok",
    
    // Mailjet
    "_hsenc",
    "_hsmi",
    
    // Drip
    "__s",
    
    // Iterable
    "cvosrc",
    "cvo_campaign",
    
    // Vero
    "vero_id",
    "vero_conv",
    
    // Common tracking
    "ref",
    "ref_",
    "referrer",
    "source",
    "src",
    "trk",
    "tracking",
    "affiliate",
    "aff_id",
    "campaign",
    "click_id",
    "clickid",
    
    // Amazon
    "tag",
    "linkCode",
    "linkId",
    "pf_rd_r",
    "pf_rd_p",
    "pd_rd_r",
    "pd_rd_w",
    "pf_rd_s",
    "pf_rd_t",
    "pf_rd_i",
    "pd_rd_wg",
    
    // Spotify
    "si",
    "context",
    
    // Reddit
    "share_id",
    "ref_source",
    
    // Misc
    "oly_enc_id",
    "oly_anon_id",
    "__twitter_impression",
    "_openstat",
    "xtor",
    NULL  // Sentinel
};

// Check if a parameter name is in the tracking list
static int is_tracking_param(const char* param, size_t len) {
    for (const char** p = tracking_params; *p != NULL; p++) {
        size_t plen = strlen(*p);
        if (len == plen && strncasecmp(param, *p, len) == 0) {
            return 1;
        }
    }
    return 0;
}

// Clean a URL by removing tracking parameters
// Returns a newly allocated string (caller must free)
char* clean_url(const char* url) {
    if (!url || !*url) {
        return strdup("");
    }
    
    size_t len = strlen(url);
    char* result = malloc(len + 1);
    if (!result) {
        return NULL;
    }
    
    // Find the query string start
    const char* query_start = strchr(url, '?');
    if (!query_start) {
        // No query string, return URL as-is (remove trailing whitespace)
        strcpy(result, url);
        char* end = result + strlen(result) - 1;
        while (end > result && isspace((unsigned char)*end)) {
            *end-- = '\0';
        }
        return result;
    }
    
    // Copy base URL (everything before '?')
    size_t base_len = query_start - url;
    memcpy(result, url, base_len);
    result[base_len] = '\0';
    
    // Find fragment (everything after '#')
    const char* fragment = strchr(query_start, '#');
    const char* query_end = fragment ? fragment : url + len;
    
    // Parse and filter query parameters
    const char* p = query_start + 1;  // Skip '?'
    int first_param = 1;
    char* out = result + base_len;
    
    while (p < query_end) {
        // Find parameter name end
        const char* eq = p;
        while (eq < query_end && *eq != '=' && *eq != '&') {
            eq++;
        }
        
        size_t param_len = eq - p;
        
        // Check if this is a tracking parameter
        int is_tracking = is_tracking_param(p, param_len);
        
        // Find end of this parameter (value included)
        const char* param_end = eq;
        if (param_end < query_end && *param_end == '=') {
            param_end++;  // Skip '='
            while (param_end < query_end && *param_end != '&') {
                param_end++;
            }
        }
        
        // Keep non-tracking parameters
        if (!is_tracking && param_len > 0) {
            *out++ = first_param ? '?' : '&';
            first_param = 0;
            
            while (p < param_end) {
                *out++ = *p++;
            }
        }
        
        // Move to next parameter
        p = param_end;
        if (p < query_end && *p == '&') {
            p++;
        }
    }
    
    // Copy fragment if present
    if (fragment) {
        // Remove trailing whitespace from fragment
        const char* frag_end = fragment + strlen(fragment);
        while (frag_end > fragment && isspace((unsigned char)*(frag_end - 1))) {
            frag_end--;
        }
        size_t frag_len = frag_end - fragment;
        memcpy(out, fragment, frag_len);
        out += frag_len;
    }
    
    *out = '\0';
    return result;
}

// Read URL from stdin
static char* read_stdin(void) {
    char* buffer = NULL;
    size_t size = 0;
    size_t capacity = 0;
    int c;
    
    while ((c = getchar()) != EOF) {
        // Ensure space for character plus null terminator
        if (size + 1 >= capacity) {
            capacity = capacity ? capacity * 2 : 256;
            char* new_buffer = realloc(buffer, capacity);
            if (!new_buffer) {
                free(buffer);
                return NULL;
            }
            buffer = new_buffer;
        }
        buffer[size++] = (char)c;
    }
    
    if (buffer) {
        buffer[size] = '\0';  // Null terminate before trimming
        // Trim whitespace
        while (size > 0 && isspace((unsigned char)buffer[size - 1])) {
            size--;
        }
        buffer[size] = '\0';
    }
    
    return buffer;
}

// Read URL from clipboard using xclip
static char* read_clipboard(void) {
    FILE* pipe = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!pipe) {
        return NULL;
    }
    
    char* buffer = NULL;
    size_t size = 0;
    size_t capacity = 0;
    int c;
    
    while ((c = fgetc(pipe)) != EOF) {
        // Ensure space for character plus null terminator
        if (size + 1 >= capacity) {
            capacity = capacity ? capacity * 2 : 256;
            char* new_buffer = realloc(buffer, capacity);
            if (!new_buffer) {
                free(buffer);
                pclose(pipe);
                return NULL;
            }
            buffer = new_buffer;
        }
        buffer[size++] = (char)c;
    }
    
    int status = pclose(pipe);
    
    if (buffer) {
        buffer[size] = '\0';  // Null terminate before trimming
        // Trim whitespace
        while (size > 0 && isspace((unsigned char)buffer[size - 1])) {
            size--;
        }
        buffer[size] = '\0';
    }
    
    if (status != 0) {
        free(buffer);
        return NULL;
    }
    
    return buffer;
}

// Write to clipboard using xclip
static int write_clipboard(const char* text) {
    FILE* pipe = popen("xclip -selection clipboard 2>/dev/null", "w");
    if (!pipe) {
        return -1;
    }
    
    fputs(text, pipe);
    return pclose(pipe);
}

static void print_usage(const char* prog) {
    fprintf(stderr, "urlclean v%s - Remove tracking parameters from URLs\n", VERSION);
    fprintf(stderr, "Usage: %s [OPTIONS] [URL]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -h, --help     Show this help message\n");
    fprintf(stderr, "  -v, --version  Show version\n");
    fprintf(stderr, "  -c, --clip     Read from clipboard and write cleaned URL back\n");
    fprintf(stderr, "  -l, --list     List all tracking parameters that are filtered\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s 'https://example.com?utm_source=test&id=123'\n", prog);
    fprintf(stderr, "  echo 'https://example.com?fbclid=abc' | %s\n", prog);
    fprintf(stderr, "  %s -c  # Clean URL in clipboard\n", prog);
}

static void print_tracking_params(void) {
    printf("Tracking parameters removed by urlclean:\n\n");
    for (const char** p = tracking_params; *p != NULL; p++) {
        printf("  %s\n", *p);
    }
}

int main(int argc, char* argv[]) {
    char* url = NULL;
    int use_clipboard = 0;
    int free_url = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("urlclean v%s\n", VERSION);
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--clip") == 0) {
            use_clipboard = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            print_tracking_params();
            return 0;
        } else if (argv[i][0] != '-') {
            url = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Get URL from source
    if (url) {
        // URL provided as argument
    } else if (use_clipboard) {
        url = read_clipboard();
        free_url = 1;
        if (!url) {
            fprintf(stderr, "Error: Could not read from clipboard. Is xclip installed?\n");
            return 1;
        }
    } else if (!isatty(STDIN_FILENO)) {
        // Read from stdin if piped
        url = read_stdin();
        free_url = 1;
        if (!url) {
            fprintf(stderr, "Error: Could not read from stdin\n");
            return 1;
        }
    } else {
        // No input, try clipboard
        url = read_clipboard();
        free_url = 1;
        if (!url) {
            print_usage(argv[0]);
            return 1;
        }
        use_clipboard = 1;  // Write back to clipboard
    }
    
    // Clean the URL
    char* cleaned = clean_url(url);
    if (!cleaned) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        if (free_url) free(url);
        return 1;
    }
    
    // Output result
    if (use_clipboard) {
        if (write_clipboard(cleaned) == 0) {
            printf("Cleaned URL copied to clipboard:\n%s\n", cleaned);
        } else {
            printf("%s\n", cleaned);
        }
    } else {
        printf("%s\n", cleaned);
    }
    
    // Cleanup
    free(cleaned);
    if (free_url) free(url);
    
    return 0;
}
