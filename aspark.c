/* aspark.c -- ASCII Sparklines
 *
 * Copyright(C) 2011 Salvatore Sanfilippo <antirez@gmail.com>
 * All rights reserved.
 *
 * Inspired by https://github.com/holman/spark
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>

#define ASPARK_MODE_ARGUMENT 0   /* read data sequence from arguments */
#define ASPARK_MODE_STREAM   1   /* read sequence of data from standard input */
#define ASPARK_MODE_TXTFREQ  2   /* compute char frequency of text */
#define ASPARK_MODE_BINFREQ  3   /* binary (all 256 bytes) frequency */

/* This is the charset used to display the graphs, but multiple rows are used
 * to increase the resolution. */
char charset[] = "_-`";
char charset_fill[] = "_o#";
int charset_len = sizeof(charset)-1;

/* Global config */
char *opt_data = NULL;
int opt_mode = ASPARK_MODE_ARGUMENT;
int opt_columns = -1; /* -1 means auto-detect number of columns. */
int opt_rows = 2;     /* Number of rows to use to increase resolution. */
int opt_label_margin_top = 1; /* Spaces before labels. */
int opt_log = 0; /* Logarithmic mode */
int opt_fill = 0; /* Fill the area under the sparkline */

/* ----------------------------------------------------------------------------
 * Sequences
 * ------------------------------------------------------------------------- */

/* A sequence is represented of many "samples" */
struct sample {
    double value;
    char *label;
};

struct sequence {
    int length;
    int labels;
    struct sample *samples;
    double min, max;
};

/* Create a new sequence. */
struct sequence *create_sequence(void) {
    struct sequence *seq = malloc(sizeof(*seq));
    seq->length = 0;
    seq->samples = NULL;
    return seq;
}

/* Add a new sample into a sequence */
void sequence_add_sample(struct sequence *seq, double value, char *label) {
    if (seq->length == 0) {
        seq->min = seq->max = value;
    } else {
        if (value < seq->min) seq->min = value;
        else if (value > seq->max) seq->max = value;
    }
    seq->samples = realloc(seq->samples,sizeof(struct sample)*(seq->length+1));
    seq->samples[seq->length].value = value;
    seq->samples[seq->length].label = label;
    seq->length++;
    if (label) seq->labels++;
}

/* ----------------------------------------------------------------------------
 * Argument mode
 * ------------------------------------------------------------------------- */

/* Convert a string in the form 1,2,3.4,5:label1,6:label2 into a sequence.
 * On error NULL is returned and errno set accordingly:
 *
 * EINVAL => Invalid data format. */
struct sequence *argument_to_sequence(const char *arg) {
    struct sequence *seq = create_sequence();
    char *copy, *start;

    start = copy = strdup(arg);
    while(*start) {
        char *label, *end, *endptr;
        double value;

        end = strchr(start,',');
        if (end) *end = '\0';
        label = strchr(start,':');
        if (label) {
            *label = '\0';
            label++; /* skip the ':' */
        }
        errno = 0;
        value = strtod(start,&endptr);
        if (*endptr != '\0' || errno != 0 || isinf(value) || isnan(value)) {
            errno = EINVAL;
            return NULL;
        }
        sequence_add_sample(seq,value,label ? strdup(label) : NULL);
        if (end)
            start = end+1;
        else
            break;
    }
    free(copy);
    return seq;
}

/* ----------------------------------------------------------------------------
 * File frequency mode
 * ------------------------------------------------------------------------- */

/* Read chars from stdin until end of file, build a frequency table, and
 * finally translate it into samples. */
struct sequence *file_freq_to_sequence(void) {
    struct sequence *seq;
    unsigned int count[256];
    int c;

    memset(count,0,sizeof(count));
    while((c = getc(stdin)) != EOF) {
        if (opt_mode == ASPARK_MODE_TXTFREQ) c = toupper(c);
        count[c]++;
    }
    seq = create_sequence();
    if (opt_mode == ASPARK_MODE_BINFREQ) {
        for (c = 0; c < 256; c++) {
            char buf[32];

            snprintf(buf,sizeof(buf),"%d",c);
            sequence_add_sample(seq,count[c],strdup(buf));
        }
    } else {
        for (c = ' '+1; c <= 'Z'; c++) {
            char buf[2];

            snprintf(buf,sizeof(buf),"%c",c);
            sequence_add_sample(seq,count[c],strdup(buf));
        }
    }
    return seq;
}

/* ----------------------------------------------------------------------------
 * File stream mode
 * ------------------------------------------------------------------------- */

/* Reads data line after line from a standard input, where every line
 * represents a single data point that can be a number or optionally
 * a number, any number of spaces, and a label, and returns the sequence
 * represented by this stream.
 *
 * The function is very tolerant about spaces and badly formatted lines. */
struct sequence *datastream_to_sequence(void) {
    struct sequence *seq = create_sequence();
    char buf[4096];

    while(fgets(buf,sizeof(buf),stdin) != NULL) {
        char *endptr, *start, *label = NULL, *p = buf;
        double value;

        /* Skip initial spaces */
        while(*p && isspace(*p)) p++;
        if (*p == '\0') continue; /* Empty line, skip it */
        start = p;
        /* Find the end of the value */
        while(*p && !isspace(*p)) p++;
        if (*p) {
            *p = '\0';
            p++;
        }
        /* Parse the value */
        errno = 0;
        value = strtod(start,&endptr);
        if (*endptr != '\0' || errno != 0) continue; /* Bad float, skip it */
        /* Find the start of the label */
        while(*p && isspace(*p)) p++;
        if (*p != '\0') {
            /* We have an additional label, find the end */
            label = p;
            while(*p && !isspace(*p)) p++;
            *p = '\0';
        }
        sequence_add_sample(seq,value,label ? strdup(label) : NULL);
    }
    return seq;
}

/* ----------------------------------------------------------------------------
 * ASCII rendering of sequence
 * ------------------------------------------------------------------------- */

/* Render part of a sequence, so that render_sequence() call call this function
 * with differnent parts in order to create the full output without overflowing
 * the current terminal columns. */
void render_sub_sequence(struct sequence *seq, int rows, int offset, int len)
{
    int j;
    double relmax = seq->max - seq->min;
    int steps = charset_len*rows;
    int row = 0;
    char *chars = malloc(len);
    int loop = 1;

    if (opt_log) {
        relmax = log(relmax+1);
    } else if (relmax == 0) {
        relmax = 1;
    }

    while(loop) {
        loop = 0;
        memset(chars,' ',len);
        for (j = 0; j < len; j++) {
            struct sample *s = &seq->samples[j+offset];
            double relval = s->value - seq->min;
            int step;

            if (opt_log) relval = log(relval+1);
            step = (int) (relval*steps)/relmax;
            if (step < 0) step = 0;
            if (step >= steps) step = steps-1;

            if (row < rows) {
                /* Print the character needed to create the sparkline */
                int charidx = step-((rows-row-1)*charset_len);
                loop = 1;
                if (charidx >= 0 && charidx < charset_len) {
                    chars[j] = opt_fill ? charset_fill[charidx] :
                                          charset[charidx];
                } else if(opt_fill && charidx >= charset_len) {
                    chars[j] = '|';
                }
            } else {
                /* Labels spacing */
                if (seq->labels && row-rows < opt_label_margin_top) {
                    loop = 1;
                    break;
                }
                /* Print the label if needed. */
                if (s->label) {
                    int label_len = strlen(s->label);
                    int label_char = row-rows-opt_label_margin_top;

                    if (label_len > label_char) {
                        loop = 1;
                        chars[j] = s->label[label_char];
                    }
                }
            }
        }
        if (loop) {
            row++;
            printf("%.*s\n", len, chars);
        }
    }
    free(chars);
}

/* Turn a sequence into its ASCII representation */
void render_sequence(struct sequence *seq, int columns, int rows) {
    int j;

    for (j = 0; j < seq->length; j += columns) {
        int sublen = (seq->length-j) < columns ? (seq->length-j) : columns;

        render_sub_sequence(seq, rows, j, sublen);
    }
}

/* ----------------------------------------------------------------------------
 * Main & Company
 * ------------------------------------------------------------------------- */

/* Show help and exit */
void show_help(void) {
    printf("Usage: spark [options] [comma separated values]\n");
    exit(0);
}

/* Arguments parsing, altering the global vars opt_*  */
void parse_args(int argc, char **argv) {
    int j;

    for (j = 1; j < argc; j++) {
        int lastarg = (j == argc-1);
        if (!strcasecmp(argv[j],"--help")) {
            show_help();
        } else if (!strcasecmp(argv[j],"--binfreq")) {
            opt_mode = ASPARK_MODE_BINFREQ;
        } else if (!strcasecmp(argv[j],"--txtfreq")) {
            opt_mode = ASPARK_MODE_TXTFREQ;
        } else if (!strcasecmp(argv[j],"--stream")) {
            opt_mode = ASPARK_MODE_STREAM;
        } else if (!strcasecmp(argv[j],"--log")) {
            opt_log = 1;
        } else if (!strcasecmp(argv[j],"--fill")) {
            opt_fill = 1;
        } else if (!strcasecmp(argv[j],"--columns") && !lastarg) {
            opt_columns = atoi(argv[++j]);
        } else if (!strcasecmp(argv[j],"--rows") && !lastarg) {
            opt_rows = atoi(argv[++j]);
        } else if (!strcasecmp(argv[j],"--label-margin-top") && !lastarg) {
            opt_label_margin_top = atoi(argv[++j]);
        } else {
            if (opt_data == NULL) {
                opt_data = argv[j];
            } else {
                fprintf(stderr,"Unrecognized option: '%s'\n", argv[j]);
                exit(1);
            }
        }
    }
    if (opt_mode != ASPARK_MODE_ARGUMENT && opt_data) {
        fprintf(stderr,"Error: data argument passed but incompatible mode selected.\n");
        exit(1);
    } else if (opt_mode == ASPARK_MODE_ARGUMENT && !opt_data) {
        fprintf(stderr,"Error: missing data.\n");
        exit(1);
    }
}

/* Try to detect the number of columns in the current terminal. */
void detect_columns(void) {
    char *columns;

    if (opt_columns != -1) return;
    columns = getenv("COLUMNS");
    opt_columns = columns ? atoi(columns) : 80;
}

/* Read the sequence accordingly to the current running mode */
struct sequence *read_sequence(void) {
    struct sequence *seq;

    if (opt_mode == ASPARK_MODE_ARGUMENT) {
        seq = argument_to_sequence(opt_data);
        if (!seq) {
            fprintf(stderr,"Bad data format: '%s'\n", opt_data);
            exit(1);
        }
    } else if (opt_mode == ASPARK_MODE_BINFREQ ||
               opt_mode == ASPARK_MODE_TXTFREQ)
    {
        seq = file_freq_to_sequence();
    } else if (opt_mode == ASPARK_MODE_STREAM) {
        seq = datastream_to_sequence();
    }
    return seq;
}

int main(int argc, char **argv) {
    struct sequence *seq;

    parse_args(argc, argv);
    detect_columns();
    seq = read_sequence();
    render_sequence(seq,opt_columns,opt_rows);
    return 0;
}
