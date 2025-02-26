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
#include "parser.h"

struct com_result
{
    int need_exit;
    int return_code;
};

static struct com_result 
exit_from_command(int need_exit, int return_code) 
{
    struct com_result res;
    res.need_exit = need_exit;
    res.return_code = return_code;

    return res;
}

char*
unquote(const char *str) 
{
    size_t len = strlen(str);
    if (len < 2) 
				return strdup(str);

    char *result = (char *)calloc(len + 1, sizeof(char));
    if (!result) 
				return NULL;

    char quote_char = 0;
    int j = 0;
    for (size_t i = 0; i < len; i++) {
        if (!quote_char && (str[i] == '"' || str[i] == '\''))
            quote_char = str[i];
        else if (quote_char == str[i])
            quote_char = 0;
        else
            result[j++] = str[i];
    }
    result[j] = '\0';

    return result;
}

int 
exec_cd(int arg_count, char** arg) 
{
    char *path;

    if (arg_count == 0) {
        path = getenv("HOME");
        if (!path)
            path = getpwuid(getuid())->pw_dir;
        
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
    if ((return_code = chdir(path)) == -1)
        dprintf(STDERR_FILENO, "failed to change directory: %s\n", strerror(errno));

    return return_code;
}

static int 
exec_exit(int arg_count, char** arg) 
{
    if (arg_count == 1)
        return atoi(arg[0]);
    else if (arg_count == 0)
        return 0;
    else
        return -1;
}

static void 
execute_cmd(const struct expr *expression) 
{
    assert(expression != NULL);

    char **args = calloc(expression->cmd.arg_count + 2, sizeof(char*));
    args[0] = expression->cmd.exe;
    memcpy(args + 1, expression->cmd.args, sizeof(char*) * expression->cmd.arg_count);

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

static bool 
is_cd_com(const struct expr *e) 
{
		assert(e != NULL);
		return strcmp(e->cmd.exe, "cd") == 0;
}

static bool 
is_exit_com(const struct expr *e) 
{
		assert(e != NULL);
		return strcmp(e->cmd.exe, "exit") == 0;
}

static bool 
is_builtin_com(const struct expr *e)
{
		assert(e != NULL);
		return is_exit_com(e) || is_cd_com(e);
}

static void 
setup_io_redirection(int out_fd) 
{
    if (out_fd != -1) {
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
    }
}

static void 
restore_io(int saved_stdout, int saved_stdin) 
{
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdout);
    close(saved_stdin);
}

static int 
open_output_file(const char *out_file, enum output_type out_type) 
{
    int flags = O_WRONLY | O_CREAT;
    if (out_type == OUTPUT_TYPE_FILE_NEW)
        flags |= O_TRUNC;
    else if (out_type == OUTPUT_TYPE_FILE_APPEND)
        flags |= O_APPEND;

    char *cleaned_out_file = unquote(out_file);
    int out_fd = open(cleaned_out_file, flags, 0644);
    if (out_fd == -1) {
        perror("open");
        free(cleaned_out_file);
        return -1;
    }
    free(cleaned_out_file);

    return out_fd;
}

static struct com_result 
execute_pipeline(const struct command_line *line) 
{
    if (line->head == NULL) return exit_from_command(0, 0);

    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stdin = dup(STDIN_FILENO);

    int prev_pipe[2] = {-1, -1};
    struct expr *e = line->head;
    int piped_count = 0;

    struct expr *count_expr = line->head;
    while (count_expr != NULL && !is_expr_logical(count_expr)) {
        if (count_expr->type == EXPR_TYPE_COMMAND)
            piped_count++;
        count_expr = count_expr->next;
    }

    int *child_pids = (int *)calloc(piped_count, sizeof(int));
    if (child_pids == NULL) {
        perror("calloc");
        return exit_from_command(0, EXIT_FAILURE);
    }

    int out_fd = -1;
    if (line->out_file != NULL) {
        out_fd = open_output_file(line->out_file, line->out_type);
        if (out_fd == -1) {
            free(child_pids);
            return exit_from_command(0, EXIT_FAILURE);
        }
    }

    int exit_code = 0;
    bool last_builtin = false;

    for (int i = 0; i < piped_count; i++) {
        int cur_pipe[2];
        if (i < piped_count - 1) {
            if (pipe(cur_pipe) == -1) {
                perror("pipe");
                free(child_pids);
                return exit_from_command(0, EXIT_FAILURE);
            }
        }

        bool is_last_command = (i == piped_count - 1);
        bool is_builtin = is_builtin_com(e);

        if (is_builtin) {
            if (is_last_command) {
                if (is_cd_com(e)) {
                    exit_code = exec_cd(e->cmd.arg_count, e->cmd.args);
                    last_builtin = true;
                } else if (is_exit_com(e)) {
                    exit_code = exec_exit(e->cmd.arg_count, e->cmd.args);
                    last_builtin = true;
                }
            } else {
                exit_code = 0;

                if (prev_pipe[0] != -1) close(prev_pipe[0]);
                if (prev_pipe[1] != -1) close(prev_pipe[1]);

                prev_pipe[0] = cur_pipe[0];
                prev_pipe[1] = cur_pipe[1];
            }
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                if (i > 0)
                    dup2(prev_pipe[0], STDIN_FILENO);
                if (i < piped_count - 1)
                    dup2(cur_pipe[1], STDOUT_FILENO);
                else if (out_fd != -1)
                    setup_io_redirection(out_fd);

                if (prev_pipe[0] != -1) close(prev_pipe[0]);
                if (prev_pipe[1] != -1) close(prev_pipe[1]);
                if (cur_pipe[0] != -1) close(cur_pipe[0]);
                if (cur_pipe[1] != -1) close(cur_pipe[1]);

                execute_cmd(e);
            } else if (pid > 0) {
                child_pids[i] = pid;

                if (prev_pipe[0] != -1) close(prev_pipe[0]);
                if (prev_pipe[1] != -1) close(prev_pipe[1]);

                prev_pipe[0] = cur_pipe[0];
                prev_pipe[1] = cur_pipe[1];
            } else {
                perror("fork");
                free(child_pids);
                return exit_from_command(0, EXIT_FAILURE);
            }
        }
        e = e->next;
        while (e != NULL && e->type != EXPR_TYPE_COMMAND)
            e = e->next;
    }

    if (prev_pipe[0] != -1) close(prev_pipe[0]);
    if (prev_pipe[1] != -1) close(prev_pipe[1]);
    if (out_fd != -1) close(out_fd);

    int last_exit_code = 0;

    for (int i = 0; i < piped_count; i++) {
        int status;
        waitpid(child_pids[i], &status, 0);
        if (WIFEXITED(status) && i == piped_count - 1)
            last_exit_code = WEXITSTATUS(status);
    }

    if (!last_builtin)
        exit_code = last_exit_code;

    free(child_pids);
    restore_io(saved_stdout, saved_stdin);

    return exit_from_command(0, exit_code);
}

static struct com_result 
execute_single_command(const struct expr *command, const char *out_file, enum output_type out_type) 
{
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stdin = dup(STDIN_FILENO);
    int exit_code = 0;

    if (out_file != NULL) {
        int out_fd = open_output_file(out_file, out_type);
        if (out_fd == -1) {
            return exit_from_command(0, EXIT_FAILURE);
        }
        setup_io_redirection(out_fd);
    }

    if (is_cd_com(command)) {
        exit_code = exec_cd(command->cmd.arg_count, command->cmd.args);
    } else if (is_exit_com(command)) {
        exit_code = exec_exit(command->cmd.arg_count, command->cmd.args);
        return exit_from_command(1, exit_code);
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            execute_cmd(command);
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
    restore_io(saved_stdout, saved_stdin);

    return exit_from_command(0, exit_code);
}

static struct com_result 
execute_command_line(const struct command_line *line) 
{
    if (line->head == NULL) 
				return exit_from_command(0, 0);

    struct expr *iter = line->head;
    struct expr *operand_start = iter;

    while (iter != NULL && !is_expr_logical(iter))
        iter = iter->next;

    int is_last = (iter == NULL);

    struct com_result prev_result;

    if (operand_start->next != NULL && operand_start->next->type == EXPR_TYPE_PIPE) {
        prev_result = execute_pipeline(line);
    } else {
        if (operand_start->type == EXPR_TYPE_COMMAND)
            prev_result = execute_single_command(operand_start, is_last ? line->out_file : NULL, is_last ? line->out_type : OUTPUT_TYPE_STDOUT);
        else
            prev_result = exit_from_command(0, 0);
    }

    if (prev_result.need_exit)
        return prev_result;

    while (iter != NULL) {
        enum expr_type op = iter->type;
        iter = iter->next;

        if ((op == EXPR_TYPE_AND && prev_result.return_code == 0) ||
            (op == EXPR_TYPE_OR && prev_result.return_code != 0)) {
            operand_start = iter;
            while (iter != NULL && !is_expr_logical(iter))
                iter = iter->next;

            is_last = (iter == NULL);

            if (operand_start->next != NULL && operand_start->next->type == EXPR_TYPE_PIPE) {
                struct command_line pipeline_line;
                pipeline_line.head = operand_start;
                pipeline_line.out_file = is_last ? line->out_file : NULL;
                pipeline_line.out_type = is_last ? line->out_type : OUTPUT_TYPE_STDOUT;

                prev_result = execute_pipeline(&pipeline_line);
            } else {
                if (operand_start->type == EXPR_TYPE_COMMAND)
                    prev_result = execute_single_command(operand_start, is_last ? line->out_file : NULL, is_last ? line->out_type : OUTPUT_TYPE_STDOUT);
                else
                    prev_result = exit_from_command(0, 0);
            }

            if (prev_result.need_exit)
                return prev_result;
        }
    }

    return prev_result;
}

int 
main(void) 
{
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
            struct com_result result = execute_command_line(line);
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