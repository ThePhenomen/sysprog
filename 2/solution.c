#include "parser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdbool.h>
#include <errno.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

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

struct exec_result {
	int need_exit;
	int return_code;
};

static struct exec_result
make_result(int need_exit, int return_code) {
	struct exec_result res;
	res.need_exit = need_exit;
	res.return_code = return_code;
	return res;
}

int exec_cd(int arg_count, char** arg) {
	char* path;

	if (arg_count == 0) {
			path = getenv("HOME");
			if (!path) {
					path = getpwuid(getuid())->pw_dir;
			}
			if (!path) {
					dprintf(STDERR_FILENO, "couldn't find user's home directory\n");
					return 1;
			}
	} else if (arg_count == 1) {
			path = arg[0];
	} else {
			dprintf(STDERR_FILENO, "too many arguments\n");
			return 1;
	}

	int return_code;
  if ( (return_code = chdir(path)) == -1) {
    dprintf(STDERR_FILENO, "failed to change directory: %s\n",strerror(errno));
  };

  return return_code;
}

static int exec_exit(int arg_count, char** arg) {
	if (arg_count == 1) {
			return atoi(arg[0]);
	}
	else if (arg_count == 0)
		return 0;
	else
		return -1;
}

static void execute_cmd(const struct expr *expression) {
	assert(expression != NULL);

	char **args = calloc(expression->cmd.arg_count + 2, sizeof(char *));
	args[0] = strdup(expression->cmd.exe);
	for (uint32_t i = 0; i < expression->cmd.arg_count; i++) {
			args[i + 1] = strdup(expression->cmd.args[i]);
	}
	args[expression->cmd.arg_count + 1] = NULL;

	execvp(expression->cmd.exe, args);
	perror("execvp");
	exit(EXIT_FAILURE);
}

static int
is_expr_logical(const struct expr *e)
{
	assert(e != NULL);
	return e->type == EXPR_TYPE_AND || e->type == EXPR_TYPE_OR;
}

static struct exec_result execute_pipeline(const struct command_line *line) {
	if (line->head == NULL) return make_result(0, 0);

	int saved_stdout = dup(STDOUT_FILENO);
	int saved_stdin = dup(STDIN_FILENO);

	int prev_pipe[2] = {-1, -1};
	struct expr *e = line->head;
	int piped_count = 0;

	struct expr *count_expr = line->head;
	while (count_expr != NULL && !is_expr_logical(count_expr)) {
			if (count_expr->type == EXPR_TYPE_COMMAND) {
					piped_count++;
			}
			count_expr = count_expr->next;
	}

	int *child_pids = (int *)calloc(piped_count, sizeof(int));
	if (child_pids == NULL) {
			perror("calloc");
			return make_result(0, EXIT_FAILURE);
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
					free(child_pids);
					return make_result(0, EXIT_FAILURE);
			}
			free(cleaned_out_file);
	}

	int exit_code = 0;
	bool last_builtin = false;

	for (int i = 0; i < piped_count; i++) {
			int cur_pipe[2];
			if (i < piped_count - 1) {
					if (pipe(cur_pipe) == -1) {
							perror("pipe");
							free(child_pids);
							return make_result(0, EXIT_FAILURE);
					}
			}

			bool is_last_command = (i == piped_count - 1);
			bool is_builtin = (strcmp(e->cmd.exe, "cd") == 0 || strcmp(e->cmd.exe, "exit") == 0);

			if (is_builtin && is_last_command) {
					if (strcmp(e->cmd.exe, "cd") == 0) {
							exit_code = exec_cd(e->cmd.arg_count, e->cmd.args);
							last_builtin = true;
					} else if (strcmp(e->cmd.exe, "exit") == 0) {
							exit_code = exec_exit(e->cmd.arg_count, e->cmd.args);
							last_builtin = true;
					}
			}

			pid_t pid = fork();
			if (pid == 0) {
					if (i > 0) {
							dup2(prev_pipe[0], STDIN_FILENO);
					}
					if (i < piped_count - 1) {
							dup2(cur_pipe[1], STDOUT_FILENO);
					} else if (out_fd != -1) {
							dup2(out_fd, STDOUT_FILENO);
					}

					if (prev_pipe[0] != -1) close(prev_pipe[0]);
					if (prev_pipe[1] != -1) close(prev_pipe[1]);
					if (cur_pipe[0] != -1) close(cur_pipe[0]);
					if (cur_pipe[1] != -1) close(cur_pipe[1]);

					if (strcmp(e->cmd.exe, "cd") == 0) {
							free(child_pids);
							exit(exec_cd(e->cmd.arg_count, e->cmd.args));
					} else if (strcmp(e->cmd.exe, "exit") == 0) {
							free(child_pids);
							exit(exec_exit(e->cmd.arg_count, e->cmd.args));
					} else {
						//free(child_pids);
							execute_cmd(e);
					}
			} else if (pid > 0) {
					child_pids[i] = pid;

					if (prev_pipe[0] != -1) close(prev_pipe[0]);
					if (prev_pipe[1] != -1) close(prev_pipe[1]);

					prev_pipe[0] = cur_pipe[0];
					prev_pipe[1] = cur_pipe[1];
			} else {
					perror("fork");
					free(child_pids);
					return make_result(0, EXIT_FAILURE);
			}

			e = e->next;
			while (e != NULL && e->type != EXPR_TYPE_COMMAND) {
					e = e->next;
			}
	}

	if (prev_pipe[0] != -1) close(prev_pipe[0]);
	if (prev_pipe[1] != -1) close(prev_pipe[1]);
	if (out_fd != -1) close(out_fd);

	int last_exit_code = 0;

	for (int i = 0; i < piped_count; i++) {
    int status;
    waitpid(child_pids[i], &status, 0);
    if (WIFEXITED(status) && i == piped_count - 1) {
        last_exit_code = WEXITSTATUS(status);
    }
	}

	if (!last_builtin) {
			exit_code = last_exit_code;
	}

	free(child_pids);
	dup2(saved_stdout, STDOUT_FILENO);
	dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdout);
	close(saved_stdin);

	return make_result(0, exit_code);
}

static struct exec_result execute_command_line(const struct command_line *line) {
	
	if (line->head == NULL) return make_result(0, 0);

	struct expr *iter = line->head;
	struct expr *operand_start = iter;

	// Находим конец текущего блока команд (до логического оператора)
	while (iter != NULL && !is_expr_logical(iter)) {
		//printf("%s", iter->cmd.exe);	
		iter = iter->next;
	}

	// Определяем, является ли текущий блок последним
	int is_last = (iter == NULL);

	// Выполняем текущий блок команд
	struct exec_result prev_result;

	if (operand_start->next != NULL && operand_start->next->type == EXPR_TYPE_PIPE) {
			// Если есть пайпы, выполняем пайплайн
			prev_result = execute_pipeline(line);
	} else {
			// Иначе выполняем одиночную команду
			if (operand_start->type == EXPR_TYPE_COMMAND) {
					int saved_stdout = dup(STDOUT_FILENO);
					int saved_stdin = dup(STDIN_FILENO);
					int exit_code = 0;

					// Обработка перенаправления вывода
					if (line->out_file != NULL && is_last) {
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
									return make_result(0, EXIT_FAILURE);
							}
							free(cleaned_out_file);

							dup2(out_fd, STDOUT_FILENO);
							close(out_fd);
					}

					// Обработка встроенных команд (cd и exit)
					if (strcmp(operand_start->cmd.exe, "cd") == 0) {
							exit_code = exec_cd(operand_start->cmd.arg_count, operand_start->cmd.args);
					} else if (strcmp(operand_start->cmd.exe, "exit") == 0) {
							exit_code = exec_exit(operand_start->cmd.arg_count, operand_start->cmd.args);
							return make_result(1, exit_code);
					} else {
							// Выполнение внешней команды
							pid_t pid = fork();
							if (pid == 0) {
									execute_cmd(operand_start);
							} else if (pid > 0) {
									int status;
									waitpid(pid, &status, 0);
									if (WIFEXITED(status)) {
											exit_code = WEXITSTATUS(status);
									}
							} else {
									perror("fork");
									exit_code = EXIT_FAILURE;
							}
					}

					// Восстанавливаем стандартные потоки ввода/вывода
					dup2(saved_stdout, STDOUT_FILENO);
					dup2(saved_stdin, STDIN_FILENO);
					close(saved_stdout);
					close(saved_stdin);

					prev_result = make_result(0, exit_code);
			} else {
					prev_result = make_result(0, 0);
			}
	}

	// Если нужно завершить программу (например, команда exit), возвращаем результат
	if (prev_result.need_exit) {
			return prev_result;
	}

	// Обрабатываем логические операторы
	while (iter != NULL) {
			enum expr_type op = iter->type; // Тип оператора (&& или ||)
			iter = iter->next; // Переходим к следующему выражению

			// Проверяем, нужно ли выполнять следующий блок команд
			if ((op == EXPR_TYPE_AND && prev_result.return_code == 0) ||
					(op == EXPR_TYPE_OR && prev_result.return_code != 0)) {
					operand_start = iter; // Начало следующего блока команд
					// Находим конец следующего блока команд
					while (iter != NULL && !is_expr_logical(iter)) {
							iter = iter->next;
					}

					// Определяем, является ли текущий блок последним
					is_last = (iter == NULL);

					// Выполняем следующий блок команд
					if (operand_start->next != NULL && operand_start->next->type == EXPR_TYPE_PIPE) {
							// Если есть пайпы, выполняем пайплайн
							struct command_line pipeline_line;
							pipeline_line.head = operand_start;
							pipeline_line.out_file = is_last ? line->out_file : NULL;
							pipeline_line.out_type = is_last ? line->out_type : OUTPUT_TYPE_STDOUT;
							//pipeline_line.is_background = line->is_background;

							prev_result = execute_pipeline(&pipeline_line);
					} else {
							// Иначе выполняем одиночную команду
							if (operand_start->type == EXPR_TYPE_COMMAND) {
									int saved_stdout = dup(STDOUT_FILENO);
									int saved_stdin = dup(STDIN_FILENO);
									int exit_code = 0;

									// Обработка перенаправления вывода
									if (line->out_file != NULL && is_last) {
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
													return make_result(0, EXIT_FAILURE);
											}
											free(cleaned_out_file);

											dup2(out_fd, STDOUT_FILENO);
											close(out_fd);
									}

									// Обработка встроенных команд (cd и exit)
									if (strcmp(operand_start->cmd.exe, "cd") == 0) {
											exit_code = exec_cd(operand_start->cmd.arg_count, operand_start->cmd.args);
									} else if (strcmp(operand_start->cmd.exe, "exit") == 0) {
											exit_code = exec_exit(operand_start->cmd.arg_count, operand_start->cmd.args);
											return make_result(1, exit_code);
									} else {
											// Выполнение внешней команды
											pid_t pid = fork();
											if (pid == 0) {
													execute_cmd(operand_start);
											} else if (pid > 0) {
													int status;
													waitpid(pid, &status, 0);
													if (WIFEXITED(status)) {
															exit_code = WEXITSTATUS(status);
													}
											} else {
													perror("fork");
													exit_code = EXIT_FAILURE;
											}
									}

									// Восстанавливаем стандартные потоки ввода/вывода
									dup2(saved_stdout, STDOUT_FILENO);
									dup2(saved_stdin, STDIN_FILENO);
									close(saved_stdout);
									close(saved_stdin);

									prev_result = make_result(0, exit_code);
							} else {
									prev_result = make_result(0, 0);
							}
					}

					// Если нужно завершить программу, возвращаем результат
					if (prev_result.need_exit) {
							return prev_result;
					}
			}
		
	}
	// Возвращаем результат выполнения последнего блока команд
	return prev_result;
}

int main(void) {
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();

	int last_retcode = 0;
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
					struct exec_result result = execute_command_line(line);
					last_retcode = result.return_code;
					command_line_delete(line);

					if (result.need_exit) {
							parser_delete(p);
							return result.return_code;
					}
			}
	}
	parser_delete(p);
	return last_retcode;
}