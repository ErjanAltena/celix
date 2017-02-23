/**
 *Licensed to the Apache Software Foundation (ASF) under one
 *or more contributor license agreements.  See the NOTICE file
 *distributed with this work for additional information
 *regarding copyright ownership.  The ASF licenses this file
 *to you under the Apache License, Version 2.0 (the
 *"License"); you may not use this file except in compliance
 *with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *Unless required by applicable law or agreed to in writing,
 *software distributed under the License is distributed on an
 *"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 *specific language governing permissions and limitations
 *under the License.
 */
/*
 * shell_tui.c
 *
 *  \date       Aug 13, 2010
 *  \author    	<a href="mailto:dev@celix.apache.org">Apache Celix Project Team</a>
 *  \copyright	Apache License, Version 2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>

#include "bundle_context.h"
#include "bundle_activator.h"
#include "linked_list.h"
#include "shell.h"
#include "shell_tui.h"
#include "utils.h"
#include <signal.h>
#include <unistd.h>
#include "history.h"

#define LINE_SIZE 256
#define PROMPT "-> "

// Local types


// static function declarations
static void remove_newlines(char* line);

static void remove_newlines(char* line) {
    for(int i = 0; i < strlen(line); i++) {
        if(line[i] == '\n') {
            for(int j = 0; j < strlen(&line[i]); j++) {
                line[i+j] = line[i+j+1];
            }
        }
    }
}

static void clearLine() {
	printf("\033[2K\r");
	fflush(stdout);
}

static void cursorLeft(int n) {
	if(n>0) {
		printf("\033[%dD", n);
		fflush(stdout);
	}
}

static void writeLine(const char*line, int pos) {
    clearLine();
    write(STDOUT_FILENO, PROMPT, strlen(PROMPT));
    write(STDOUT_FILENO, line, strlen(line));
	cursorLeft(strlen(line)-pos);
}

static void* shellTui_runnable(void *data) {
    shell_tui_activator_pt act = (shell_tui_activator_pt) data;

    char in[LINE_SIZE] = "";
    char buffer[LINE_SIZE];
    int pos = 0;
    char dline[LINE_SIZE];
    fd_set rfds;
    struct timeval tv;

    struct termios term_org, term_new;
    tcgetattr(STDIN_FILENO, &term_org);
    term_new = term_org;
    term_new.c_lflag &= ~( ICANON | ECHO);
    tcsetattr(STDIN_FILENO,  TCSANOW, &term_new);

    history_t *hist = historyCreate();

    while (act->running) {
        char * line = NULL;
        writeLine(in, pos);
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(1, &rfds, NULL, NULL, &tv) > 0) {
        	int nr_chars = read(STDIN_FILENO, buffer, LINE_SIZE-pos);
            for(int bufpos = 0; bufpos < nr_chars; bufpos++) {
                if (buffer[bufpos] == '\033' && buffer[bufpos+1] == '[') { //Escape ch  \033 + [ +  A/B/C/D
                    switch (buffer[bufpos+2]) {
                        case 'A': // Arrow up
                            if(historySize(hist) > 0) {
                                strncpy(in, historyGetPrevLine(hist), LINE_SIZE);
                                pos = strlen(in);
                                writeLine(in, pos);
                            }
                            break;
                        case 'B': // Arrow Down
                            if(historySize(hist) > 0) {
                                strncpy(in, historyGetNextLine(hist), LINE_SIZE);
                                pos = strlen(in);
                                writeLine(in, pos);
                            }
                            break;
                        case 'C': //Arrow right
                            if (pos < strlen(in)) {
                        		pos++;
                        	}
                    		writeLine(in, pos);
                        	break;
                        case 'D': //Arrow left
                        	if (pos > 0) {
                            	pos--;
                            }
                    		writeLine(in, pos);
                            break;
                        case '3':
                        	if(buffer[bufpos+3] == '~') {
                        		// delete char
                                bufpos++; // delete cmd takes 4 chars
                                int len = strlen(in);
                                if (pos < len) {
                                	for (int i = pos; i <= len; i++) {
                                		in[i] = in[i + 1];
                                	}
                                }
                                writeLine(in, pos);
                        	}
                        	break;
                        default:
                            printf("Unknown %d : %c\n", nr_chars, buffer[bufpos+2]);
                            break;
                    }
                    bufpos+=2;
                    continue;
                } else if(buffer[bufpos] == 127) { // backspace
                	if(pos > 0) {
                		int len = strlen(in);
                		for(int i = pos-1; i <= len; i++) {
                			in[i] = in[i+1];
                		}
                		pos--;
                	}
            		writeLine(in, pos);
            		continue;
                } else if(buffer[bufpos] != '\n') { // Normal text
                	if(in[pos] == '\0') {
                    	in[pos+1] = '\0';
                	}
                	in[pos] = buffer[bufpos];
                    pos++;
            		writeLine(in, pos);
                	continue;
                }
        		writeLine(in, pos);
                write(STDOUT_FILENO, "\n", 1);
                remove_newlines(in);
                history_addLine(hist, in);

                memset(dline, 0, LINE_SIZE);
                strncpy(dline, in, LINE_SIZE);

                pos = 0;
                in[pos] = '\0';

                line = utils_stringTrim(dline);
                if ((strlen(line) == 0) || (act->shell == NULL)) {
                    continue;
                }
                historyLineReset(hist);
                celixThreadMutex_lock(&act->mutex);
                if (act->shell != NULL) {
                    act->shell->executeCommand(act->shell->shell, line, stdout, stderr);
                    pos = 0;
                    nr_chars = 0;
                    celixThreadMutex_unlock(&act->mutex);
                    break;
                } else {
                    fprintf(stderr, "Shell service not available\n");
                }
                celixThreadMutex_unlock(&act->mutex);
            } // for
        }
    }
    tcsetattr(STDIN_FILENO,  TCSANOW, &term_org);
    historyDestroy(hist);

    return NULL;
}

celix_status_t shellTui_start(shell_tui_activator_pt activator) {

    celix_status_t status = CELIX_SUCCESS;

    activator->running = true;
    celixThread_create(&activator->runnable, NULL, shellTui_runnable, activator);

    return status;
}

celix_status_t shellTui_stop(shell_tui_activator_pt act) {
    celix_status_t status = CELIX_SUCCESS;
    act->running = false;
    celixThread_kill(act->runnable, SIGUSR1);
    celixThread_join(act->runnable, NULL);
    return status;
}

