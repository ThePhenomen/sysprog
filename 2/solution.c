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

// int
// exec_exit(int arg_count, char** arg) 
// {
//   if (arg_count == 0) {
//     exit(0);
//     return 0;
//   }
//   else if (arg_count == 1) {
//     int exit_code = atoi(arg[0]);
//     exit(exit_code);
//     return(exit_code);
//   }
//   else {
//     dprintf(STDERR_FILENO, "too many arguments\n");
//     return -1;
//   };
// }
int exec_exit(int arg_count, char** arg) {
	if (arg_count == 0) {
			return 0;
	}
	else if (arg_count == 1) {
			return atoi(arg[0]);
	}
	else {
			dprintf(STDERR_FILENO, "too many arguments\n");
			return -1;
	}
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

// 	// Подсчитываем количество команд в пайплайне
// 	struct expr *count_expr = line->head;
// 	while (count_expr != NULL) {
// 			if (count_expr->type == EXPR_TYPE_COMMAND) {
// 					piped_count++;
// 			}
// 			count_expr = count_expr->next;
// 	}

// 	child_pids = (int *)calloc(piped_count, sizeof(int));
// 	if (child_pids == NULL) {
// 			perror("calloc");
// 			return EXIT_FAILURE;
// 	}

// 	// Подготовка для перенаправления вывода
// 	int out_fd = -1;
// 	if (line->out_file != NULL) {
// 			int flags = O_WRONLY | O_CREAT;
// 			if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
// 					flags |= O_TRUNC;
// 			} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
// 					flags |= O_APPEND;
// 			}

// 			char *cleaned_out_file = unquote(line->out_file);
// 			out_fd = open(cleaned_out_file, flags, 0644);
// 			if (out_fd == -1) {
// 					perror("open");
// 					free(cleaned_out_file);
// 					return EXIT_FAILURE;
// 			}
// 			free(cleaned_out_file);

// 			// Если всего одна команда без пайпа, перенаправляем вывод в родительском процессе
// 			if (piped_count == 1) {
// 					dup2(out_fd, STDOUT_FILENO);
// 					close(out_fd);
// 			}
// 	}

// 	// Обработка всех команд, кроме последней
// 	for (int i = 0; i < piped_count - 1; i++) {
// 			int cur_pipe[2] = {-1, -1};
// 			if (pipe(cur_pipe) == -1) {
// 					perror("pipe");
// 					free(child_pids);
// 					return EXIT_FAILURE;
// 			}

// 			bool is_cd = (strcmp(e->cmd.exe, "cd") == 0);
// 			bool is_exit = (strcmp(e->cmd.exe, "exit") == 0);

// 			pid_t pid = fork();
// 			if (pid == 0) {
// 					// Дочерний процесс

// 					// Подключаем stdin к предыдущему пайпу, если это не первая команда
// 					if (i > 0) {
// 							dup2(prev_pipe[0], STDIN_FILENO);
// 					}

// 					// Подключаем stdout к текущему пайпу
// 					dup2(cur_pipe[1], STDOUT_FILENO);

// 					// Закрываем лишние дескрипторы
// 					if (prev_pipe[0] != -1) close(prev_pipe[0]);
// 					if (prev_pipe[1] != -1) close(prev_pipe[1]);
// 					if (cur_pipe[0] != -1) close(cur_pipe[0]);
// 					if (cur_pipe[1] != -1) close(cur_pipe[1]);

// 					if (is_exit) {
// 							// Если это команда exit, завершаем процесс с указанным кодом
// 							int exit_code = (e->cmd.arg_count > 0) ? atoi(e->cmd.args[0]) : 0;
// 							_exit(exit_code);
// 					}

// 					if (is_cd) {
// 							// Если это команда cd, завершаем процесс с кодом завершения
// 							int status = exec_cd(e->cmd.arg_count, e->cmd.args);
// 							_exit(status);
// 					}

// 					// Выполнение команды
// 					int argv_count = e->cmd.arg_count + 2;
// 					char **argv = (char **)calloc(argv_count, sizeof(char *));
// 					argv[0] = strdup(e->cmd.exe);
// 					argv[argv_count - 1] = NULL;
// 					for (uint32_t j = 0; j < e->cmd.arg_count; j++) {
// 							argv[j + 1] = unquote(e->cmd.args[j]);
// 					}

// 					execvp(e->cmd.exe, argv);
// 					perror("execvp");
// 					_exit(EXIT_FAILURE);
// 			} else if (pid > 0) {
// 					child_pids[i] = pid;
// 					if (prev_pipe[0] != -1) close(prev_pipe[0]);
// 					if (prev_pipe[1] != -1) close(prev_pipe[1]);
// 					prev_pipe[0] = cur_pipe[0];
// 					prev_pipe[1] = cur_pipe[1];
// 			} else {
// 					perror("fork");
// 					free(child_pids);
// 					return EXIT_FAILURE;
// 			}

// 			e = e->next;
// 			while (e != NULL && e->type != EXPR_TYPE_COMMAND) {
// 					e = e->next;
// 			}
// 	}

// 	// Обработка последней команды
// 	int exit_code = EXIT_SUCCESS;
// 	if (piped_count > 0) {
// 			pid_t last_pid = fork();
// 			if (last_pid == 0) {
// 					// Дочерний процесс для последней команды

// 					// Подключаем stdin к предыдущему пайпу
// 					if (piped_count > 1) {
// 							dup2(prev_pipe[0], STDIN_FILENO);
// 					}

// 					// Подключаем stdout к файлу, если указано
// 					if (out_fd != -1) {
// 							dup2(out_fd, STDOUT_FILENO);
// 							close(out_fd);
// 					}

// 					// Закрываем лишние дескрипторы
// 					if (prev_pipe[0] != -1) close(prev_pipe[0]);
// 					if (prev_pipe[1] != -1) close(prev_pipe[1]);

// 					if (strcmp(e->cmd.exe, "cd") == 0) {
// 							int status = exec_cd(e->cmd.arg_count, e->cmd.args);
// 							_exit(status);
// 					} else if (strcmp(e->cmd.exe, "exit") == 0) {
// 							int exit_code = (e->cmd.arg_count > 0) ? atoi(e->cmd.args[0]) : 0;
// 							_exit(exit_code);
// 					}

// 					// Выполнение команды
// 					int argv_count = e->cmd.arg_count + 2;
// 					char **argv = (char **)calloc(argv_count, sizeof(char *));
// 					argv[0] = strdup(e->cmd.exe);
// 					argv[argv_count - 1] = NULL;
// 					for (uint32_t j = 0; j < e->cmd.arg_count; j++) {
// 							argv[j + 1] = unquote(e->cmd.args[j]);
// 					}

// 					execvp(e->cmd.exe, argv);
// 					perror("execvp");
// 					_exit(EXIT_FAILURE);
// 			} else if (last_pid > 0) {
// 					// Ожидание завершения последней команды
// 					int status;
// 					waitpid(last_pid, &status, 0);
// 					if (WIFEXITED(status)) {
// 							exit_code = WEXITSTATUS(status);
// 					}
// 			} else {
// 					perror("fork");
// 					free(child_pids);
// 					return EXIT_FAILURE;
// 			}
// 	}

// 	// Ожидание завершения всех дочерних процессов
// 	for (int i = 0; i < piped_count - 1; i++) {
// 			int status;
// 			waitpid(child_pids[i], &status, 0);
// 	}

// 	free(child_pids);
// 	dup2(saved_stdout, STDOUT_FILENO);
// 	dup2(saved_stdin, STDIN_FILENO);
// 	close(saved_stdout);
// 	close(saved_stdin);

// 	return exit_code;
// }


/*
static int execute_pipeline(const struct command_line *line) {
	if (line->head == NULL) return 0;

	int saved_stdout = dup(STDOUT_FILENO); 
	int saved_stdin = dup(STDIN_FILENO); 

	int prev_pipe[2] = {-1, -1}; 
	struct expr *e = line->head;
	int *child_pids = NULL;
	int piped_count = 0;
	int exit_code = EXIT_SUCCESS;

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
	}

	for (int i = 0; i < piped_count - 1; i++) {
			int cur_pipe[2] = {-1, -1};
			if (pipe(cur_pipe) == -1) {
					perror("pipe");
					free(child_pids);
					return EXIT_FAILURE;
			}

			pid_t pid = fork();
			if (pid == 0) {
					if (i > 0) {
							dup2(prev_pipe[0], STDIN_FILENO);
					}

					dup2(cur_pipe[1], STDOUT_FILENO);

					if (prev_pipe[0] != -1) close(prev_pipe[0]);
					if (prev_pipe[1] != -1) close(prev_pipe[1]);
					close(cur_pipe[0]);
					close(cur_pipe[1]);

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

	if (piped_count > 0) {
			pid_t last_pid = fork();
			if (last_pid == 0) {
					if (piped_count > 1) {
							dup2(prev_pipe[0], STDIN_FILENO);
					}
					if (out_fd != -1) {
							dup2(out_fd, STDOUT_FILENO);
							close(out_fd);
					}
					if (prev_pipe[0] != -1) close(prev_pipe[0]);
					if (prev_pipe[1] != -1) close(prev_pipe[1]);

					if (strcmp(e->cmd.exe, "cd") == 0) {
							int status = exec_cd(e->cmd.arg_count, e->cmd.args);
							_exit(status);
					} else if (strcmp(e->cmd.exe, "exit") == 0) {
							int exit_code = (e->cmd.arg_count > 0) ? atoi(e->cmd.args[0]) : 0;
							_exit(exit_code);
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
			} else if (last_pid > 0) {
					int status;
					waitpid(last_pid, &status, 0);
					if (WIFEXITED(status)) {
							exit_code = WEXITSTATUS(status);
					}
			} else {
					perror("fork");
					free(child_pids);
					return EXIT_FAILURE;
			}
	}

	for (int i = 0; i < piped_count - 1; i++) {
			int status;
			waitpid(child_pids[i], &status, 0);
	}

	free(child_pids);
	dup2(saved_stdout, STDOUT_FILENO);
	dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdout);
	close(saved_stdin);

	return exit_code;
};
*/






static int execute_pipeline(const struct command_line *line) {
	if (line->head == NULL) return 0;

	int saved_stdout = dup(STDOUT_FILENO);
	int saved_stdin = dup(STDIN_FILENO);

	int prev_pipe[2] = {-1, -1};
	struct expr *e = line->head;
	int piped_count = 0;

	// Подсчет количества команд в пайплайне
	struct expr *count_expr = line->head;
	while (count_expr != NULL) {
			if (count_expr->type == EXPR_TYPE_COMMAND) {
					piped_count++;
			}
			count_expr = count_expr->next;
	}

	int *child_pids = (int *)calloc(piped_count, sizeof(int));
	if (child_pids == NULL) {
			perror("calloc");
			return EXIT_FAILURE;
	}

	// Подготовка для перенаправления вывода
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
	}

	int exit_code;
	bool last_builtin = false;

	// Обработка всех команд в пайплайне
	for (int i = 0; i < piped_count; i++) {
			int cur_pipe[2];
			if (i < piped_count - 1) {
					if (pipe(cur_pipe) == -1) {
							perror("pipe");
							free(child_pids);
							return EXIT_FAILURE;
					}
			}



			bool is_last_command = (i == piped_count - 1);
			bool is_builtin = (strcmp(e->cmd.exe, "cd") == 0 || strcmp(e->cmd.exe, "exit") == 0);

			
			if (is_builtin && is_last_command) {
				if (strcmp(e->cmd.exe, "cd") == 0) {
					dup2(prev_pipe[0], STDIN_FILENO);
					exit_code = exec_cd(e->cmd.arg_count, e->cmd.args);
					last_builtin = true;
					if (exit_code != 0) {
						fprintf(stderr, "cd: failed to change directory\n");
					}
					continue;
				} else if (strcmp(e->cmd.exe, "exit") == 0) {
					dup2(prev_pipe[0], STDIN_FILENO);
					exit_code = exec_exit(e->cmd.arg_count, e->cmd.args);
					last_builtin = true;
					//exit(exit_code);
					continue;
					goto cleanup;
				}
			}

			pid_t pid = fork();
			if (pid == 0) {
					// Дочерний процесс

					// Подключаем stdin к предыдущему пайпу
					if (i > 0) {
							dup2(prev_pipe[0], STDIN_FILENO);
					}

					// Подключаем stdout к текущему пайпу, если это не последняя команда
					if (i < piped_count - 1) {
							dup2(cur_pipe[1], STDOUT_FILENO);
					} else if (out_fd != -1) {
							// Последняя команда с перенаправлением в файл
							dup2(out_fd, STDOUT_FILENO);
					}

					// Закрываем лишние дескрипторы
					if (prev_pipe[0] != -1) close(prev_pipe[0]);
					if (prev_pipe[1] != -1) close(prev_pipe[1]);
					if (cur_pipe[0] != -1) close(cur_pipe[0]);
					if (cur_pipe[1] != -1) close(cur_pipe[1]);

					// Выполнение встроенных команд в дочернем процессе
					if (strcmp(e->cmd.exe, "cd") == 0) {
							exit(exec_cd(e->cmd.arg_count, e->cmd.args));
					}
					if (strcmp(e->cmd.exe, "exit") == 0) {
							exit(exec_exit(e->cmd.arg_count, e->cmd.args));
					}

					// Выполнение внешней команды
					int argv_count = e->cmd.arg_count + 2;
					char **argv = (char **)calloc(argv_count, sizeof(char *));
					argv[0] = strdup(e->cmd.exe);
					argv[argv_count - 1] = NULL;
					for (uint32_t j = 0; j < e->cmd.arg_count; j++) {
							argv[j + 1] = unquote(e->cmd.args[j]);
					}

					execvp(argv[0], argv);
					perror("execvp");
					exit(EXIT_FAILURE);
			} else if (pid > 0) {
					child_pids[i] = pid;

					// Родительский процесс закрывает лишние дескрипторы
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

	// Закрываем последний пайп в родительском процессе
	if (prev_pipe[0] != -1) close(prev_pipe[0]);
	if (prev_pipe[1] != -1) close(prev_pipe[1]);
	if (out_fd != -1) close(out_fd);

	// Ожидание завершения всех дочерних процессов
	int last_exit_code;
	int counter = 0;
	if (last_builtin == true) { 
		counter = piped_count - 1;
	}
	else { 
		counter = piped_count;
	}

	for (int i = 0; i < counter; i++) {
			int status;
			waitpid(child_pids[i], &status, 0);
			if (WIFEXITED(status)) {
					last_exit_code = WEXITSTATUS(status);
			}
	}

	if (last_builtin == false) {
		exit_code = last_exit_code;
	}
	
cleanup:
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
							exit(exec_exit(e->cmd.arg_count, e->cmd.args));
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
