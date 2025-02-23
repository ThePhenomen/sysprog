#include "parser.h"

#include <assert.h>
#include <stdio.h>

#include <stdlib.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdbool.h>

#include <errno.h>
#include <pwd.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

int
exec_cd(int arg_count, char** arg) 
{ 
  char* path;

  if (arg_count == 0) {
    path = getenv("HOME");

    if ((path = getenv("HOME")) == NULL) {
      path = getpwuid(getuid())->pw_dir;
    };

    if (!path) {
      dprintf(STDERR_FILENO, "couldn't find user's home directory\n");
			return 1;
    };

  }
  else if (arg_count == 1) {
    path = arg[0];
  }
  else {
    dprintf(STDERR_FILENO, "too many arguments\n");
    return 1;
  };

  int return_code;
  if ( (return_code = chdir(path)) == -1) {
    dprintf(STDERR_FILENO, "failed to change directory: %s\n",strerror(errno));
    //return -1;
  };

  return return_code;
}

int
exec_exit(int arg_count, char** arg) 
{
  if (arg_count == 0) {
    exit(0);
    return 0;
  }
  else if (arg_count == 1) {
    int exit_code = atoi(arg[0]);
    exit(exit_code);
    return(exit_code);
  }
  else {
    dprintf(STDERR_FILENO, "too many arguments\n");
    return -1;
  };
}


char *unquote(const char *str) {
	size_t len = strlen(str);
	if (len < 2) return strdup(str); 

	char *result = (char *)calloc(len + 1, sizeof(char));
	char quote_char = 0;

	int j = 0;
	for (size_t i = 0; i < len; i++) {
			if ((str[i] == '"' || str[i] == '\'') && (i == 0 || i == len - 1)) {
					quote_char = str[i];
			} else if (str[i] == '\\' && (i + 1 < len) && (str[i + 1] == quote_char || str[i + 1] == '\\')) {
					result[j++] = str[++i];
			} else {
					result[j++] = str[i];
			}
	}
	return result;
}

// static int execute_pipeline(const struct command_line *line) {
// 	if (line->head == NULL) return 0;

// 	int saved_stdout = dup(STDOUT_FILENO); 
// 	int saved_stdin = dup(STDIN_FILENO); 

// 	int prev_pipe[2] = {-1, -1}; 
// 	struct expr *e = line->head;
// 	int *child_pids = NULL;
// 	int piped_count = 0;

// 	struct expr *count_expr = line->head;
// 	while (count_expr != NULL) {
// 		if (count_expr->type == EXPR_TYPE_COMMAND) {
// 			piped_count++;
// 		}
// 		count_expr = count_expr->next;
// 	}

// 	child_pids = (int *)calloc(piped_count, sizeof(int));
// 	if (child_pids == NULL) {
// 		perror("calloc");
// 		return EXIT_FAILURE;
// 	}

// 	int out_fd = -1;
// 	if (line->out_file != NULL) {
// 		int flags = O_WRONLY | O_CREAT;
// 		if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
// 			flags |= O_TRUNC;
// 		} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
// 			flags |= O_APPEND;
// 		}

// 		char *cleaned_out_file = unquote(line->out_file);
// 		out_fd = open(cleaned_out_file, flags, 0644);
// 		if (out_fd == -1) {
// 			perror("open");
// 			free(cleaned_out_file);
// 			return EXIT_FAILURE;
// 		}
// 		free(cleaned_out_file);

// 		if (piped_count == 1) {
// 			dup2(out_fd, STDOUT_FILENO);
// 			close(out_fd);
// 		}
// 	}

// 	for (int i = 0; i < piped_count; i++) {
// 		int cur_pipe[2] = {-1, -1};
// 		if (i < piped_count - 1) {
// 			if (pipe(cur_pipe) == -1) {
// 				perror("pipe");
// 				free(child_pids);
// 				return EXIT_FAILURE;
// 			}
// 		}


// 		/* Убрал чтобы проходило первый тест, нужно оставить на будущее */
// 		bool is_last_command = (i == piped_count - 1);
// 		bool is_builtin = (strcmp(e->cmd.exe, "cd") == 0 || strcmp(e->cmd.exe, "exit") == 0);

// 		if (is_builtin && is_last_command) {
// 			if (strcmp(e->cmd.exe, "cd") == 0) {
// 				int status = exec_cd(e->cmd.arg_count, e->cmd.args);
// 				if (status != 0) {
// 					fprintf(stderr, "cd: failed to change directory\n");
// 				}
// 				continue;
// 			} else if (strcmp(e->cmd.exe, "exit") == 0) {
// 				int exit_code = (e->cmd.arg_count > 0) ? atoi(e->cmd.args[0]) : 0;
// 				exit(exit_code);
// 			}
// 		}

// 		//if (is_builtin && !is_last_command) {
// 			//fprintf(stderr, "Warning: '%s' in the middle of a pipeline has no effect and is ignored\n", e->cmd.exe);
// 			//continue;
// 		//}

// 		pid_t pid = fork();
// 		if (pid == 0) {

// 			if (i > 0) {
// 				dup2(prev_pipe[0], STDIN_FILENO);
// 			}

// 			if (i < piped_count - 1) {
// 				dup2(cur_pipe[1], STDOUT_FILENO);
// 			} else if (piped_count > 1 && out_fd != -1) {
// 				dup2(out_fd, STDOUT_FILENO);
// 				close(out_fd);
// 			}

// 			if (prev_pipe[0] != -1) close(prev_pipe[0]);
// 			if (prev_pipe[1] != -1) close(prev_pipe[1]);
// 			if (cur_pipe[0] != -1) close(cur_pipe[0]);
// 			if (cur_pipe[1] != -1) close(cur_pipe[1]);
			

// 			if (strcmp(e->cmd.exe, "cd") == 0) {
// 					exit(exec_cd(e->cmd.arg_count, e->cmd.args));
// 			} else if (strcmp(e->cmd.exe, "exit") == 0) {
// 					exit(exec_exit(e->cmd.arg_count, e->cmd.args));
// 			}	
// 			int argv_count = e->cmd.arg_count + 2;
// 			char **argv = (char **)calloc(argv_count, sizeof(char *));
// 			argv[0] = strdup(e->cmd.exe);
// 			argv[argv_count - 1] = NULL;
// 			for (uint32_t j = 0; j < e->cmd.arg_count; j++) {
// 				argv[j + 1] = unquote(e->cmd.args[j]);
// 			}

// 			execvp(e->cmd.exe, argv);
// 			perror("execvp");
// 			exit(EXIT_FAILURE);
// 		} else if (pid > 0) {
// 			child_pids[i] = pid;
// 			if (prev_pipe[0] != -1) close(prev_pipe[0]);
// 			if (prev_pipe[1] != -1) close(prev_pipe[1]);
// 			prev_pipe[0] = cur_pipe[0];
// 			prev_pipe[1] = cur_pipe[1];
// 		} else {
// 			perror("fork");
// 			free(child_pids);
// 			return EXIT_FAILURE;
// 		}

// 		e = e->next;
// 		while (e != NULL && e->type != EXPR_TYPE_COMMAND) {
// 			e = e->next;
// 		}
// 	}

// 	// for (int i = 0; i < piped_count; i++) {
// 	// 	int status;
// 	// 	waitpid(child_pids[i], &status, 0);
// 	// }

// 	// free(child_pids);
// 	// dup2(saved_stdout, STDOUT_FILENO);
// 	// dup2(saved_stdin, STDIN_FILENO);
// 	// close(saved_stdout);
// 	// close(saved_stdin);

// 	// return EXIT_SUCCESS;
// 	int exit_code = EXIT_SUCCESS; 

// 	for (int i = 0; i < piped_count; i++) {
// 		int status;
// 		waitpid(child_pids[i], &status, 0);
// 		if (WIFEXITED(status)) {
// 			exit_code = WEXITSTATUS(status); 
// 		}
// 	}

// 	dup2(saved_stdout, STDOUT_FILENO);
// 	dup2(saved_stdin, STDIN_FILENO);
// 	close(saved_stdout);
// 	close(saved_stdin);

// 	free(child_pids);

// 	return exit_code;
// }

static int execute_pipeline(const struct command_line *line) {
	if (line->head == NULL) return 0;

	int saved_stdout = dup(STDOUT_FILENO); 
	int saved_stdin = dup(STDIN_FILENO); 

	int prev_pipe[2] = {-1, -1}; 
	struct expr *e = line->head;
	int *child_pids = NULL;
	int piped_count = 0;

	struct expr *count_expr = line->head;
	while (count_expr != NULL) {
			if (count_expr->type == EXPR_TYPE_COMMAND) {
					piped_count++;
			}
			count_expr = count_expr->next;
	}

	child_pids = (int *)calloc(piped_count, sizeof(int));
	if (child_pids == NULL) {
			perror("calloc");
			return EXIT_FAILURE;
	}

	int out_fd = -1;
	if (line->out_file != NULL) {
			int flags = O_WRONLY | O_CREAT;
			if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
					flags |= O_TRUNC;
			} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
					flags |= O_APPEND;
			}

			char *cleaned_out_file = unquote(line->out_file);
			out_fd = open(cleaned_out_file, flags, 0644);
			if (out_fd == -1) {
					perror("open");
					free(cleaned_out_file);
					return EXIT_FAILURE;
			}
			free(cleaned_out_file);

			if (piped_count == 1) {
					dup2(out_fd, STDOUT_FILENO);
					close(out_fd);
			}
	}

	for (int i = 0; i < piped_count; i++) {
			int cur_pipe[2] = {-1, -1};
			if (i < piped_count - 1) {
					if (pipe(cur_pipe) == -1) {
							perror("pipe");
							free(child_pids);
							return EXIT_FAILURE;
					}
			}

			bool is_cd = (strcmp(e->cmd.exe, "cd") == 0);
			bool is_exit = (strcmp(e->cmd.exe, "exit") == 0);

			pid_t pid = fork();
			if (pid == 0) {

					if (i > 0) {
							dup2(prev_pipe[0], STDIN_FILENO);
					}

					if (i < piped_count - 1) {
							dup2(cur_pipe[1], STDOUT_FILENO);
					} else if (piped_count > 1 && out_fd != -1) {
							dup2(out_fd, STDOUT_FILENO);
							close(out_fd);
					}

					if (prev_pipe[0] != -1) close(prev_pipe[0]);
					if (prev_pipe[1] != -1) close(prev_pipe[1]);
					if (cur_pipe[0] != -1) close(cur_pipe[0]);
					if (cur_pipe[1] != -1) close(cur_pipe[1]);

					if (is_exit) {
							int exit_code = exec_exit(e->cmd.arg_count, e->cmd.args);
							exit(exit_code);
					}

					if (is_cd) {
						int exit_code = exec_cd(e->cmd.arg_count, e->cmd.args);
						exit(exit_code);
					}

					int argv_count = e->cmd.arg_count + 2;
					char **argv = (char **)calloc(argv_count, sizeof(char *));
					argv[0] = strdup(e->cmd.exe);
					argv[argv_count - 1] = NULL;
					for (uint32_t j = 0; j < e->cmd.arg_count; j++) {
							argv[j + 1] = unquote(e->cmd.args[j]);
					}

					execvp(e->cmd.exe, argv);
					perror("execvp");
					_exit(EXIT_FAILURE);
			} else if (pid > 0) {
					child_pids[i] = pid;
					if (prev_pipe[0] != -1) close(prev_pipe[0]);
					if (prev_pipe[1] != -1) close(prev_pipe[1]);
					prev_pipe[0] = cur_pipe[0];
					prev_pipe[1] = cur_pipe[1];
			} else {
					perror("fork");
					free(child_pids);
					return EXIT_FAILURE;
			}

			e = e->next;
			while (e != NULL && e->type != EXPR_TYPE_COMMAND) {
					e = e->next;
			}
	}

	int exit_code = EXIT_SUCCESS;
	for (int i = 0; i < piped_count; i++) {
			int status;
			waitpid(child_pids[i], &status, 0);
			if (WIFEXITED(status)) {
					exit_code = WEXITSTATUS(status);
					if (strcmp(line->head->cmd.exe, "exit") == 0) {
							break;
					}
			}
	}

	free(child_pids);
	dup2(saved_stdout, STDOUT_FILENO);
	dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdout);
	close(saved_stdin);

	return exit_code;
}

static void execute_command_line(const struct command_line *line) {
	if (line->head == NULL) return;

	struct expr *e = line->head;
	//int last_status = 0;

	if (line->head->next != NULL && line->head->next->type == EXPR_TYPE_PIPE) {
			execute_pipeline(line);
			return;
	}		

	while (e != NULL) {
			if (e->type == EXPR_TYPE_COMMAND) {

					int saved_stdout = dup(STDOUT_FILENO); 
					int saved_stdin = dup(STDIN_FILENO);

					if (line->out_file != NULL && line->head->next == NULL) {
						int flags = O_WRONLY | O_CREAT;
						if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
								flags |= O_TRUNC;
						} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
								flags |= O_APPEND;
						}

						char *cleaned_out_file = unquote(line->out_file);
						int out_fd = open(cleaned_out_file, flags, 0644);
						if (out_fd == -1) {
								perror("open");
								free(cleaned_out_file);
								return;
						}
						free(cleaned_out_file);

						dup2(out_fd, STDOUT_FILENO);
						close(out_fd);
					}

					if (strcmp(e->cmd.exe, "cd") == 0) {
							exec_cd(e->cmd.arg_count, e->cmd.args);
							return;
					} else if (strcmp(e->cmd.exe, "exit") == 0) {
							exec_exit(e->cmd.arg_count, e->cmd.args);
							return;
					} else {
							pid_t pid = fork();
							if (pid == 0) {
									int argv_count = e->cmd.arg_count + 2; 
									char **argv = (char **)calloc(argv_count, sizeof(char *));
									argv[0] = strdup(e->cmd.exe);
									argv[argv_count - 1] = NULL;
									for (uint32_t i = 0; i < e->cmd.arg_count; i++) {
											argv[i + 1] = strdup(e->cmd.args[i]);
									}
									execvp(e->cmd.exe, argv);
									perror("execvp");
									exit(EXIT_FAILURE);
							} else if (pid > 0) {
									if (!line->is_background) {
											int status;
											waitpid(pid, &status, 0);
											//last_status = WEXITSTATUS(status);
									}
							} else {
									perror("fork");
							}
							dup2(saved_stdout, STDOUT_FILENO);
							dup2(saved_stdin, STDIN_FILENO);
							close(saved_stdout);
							close(saved_stdin);
						
							//return EXIT_SUCCESS;
					}
			}
			e = e->next;
	}
}

int main(void) {
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
			parser_feed(p, buf, rc);
			struct command_line *line = NULL;
			while (true) {
					enum parser_error err = parser_pop_next(p, &line);
					if (err == PARSER_ERR_NONE && line == NULL)
							break;
					if (err != PARSER_ERR_NONE) {
							printf("Error: %d\n", (int)err);
							continue;
					}
					execute_command_line(line);
					command_line_delete(line);
			}
	}
	parser_delete(p);
	return 0;
}
