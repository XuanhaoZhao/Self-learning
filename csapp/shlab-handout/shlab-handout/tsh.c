/*
 * tsh - 一个带有作业控制的微型shell程序
 *
 * <在这里输入你的名字和登录ID>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* 其他常数 */
#define MAXLINE    1024   /* 最大行大小 */
#define MAXARGS     128   /* 命令行上的最大参数数量 */
#define MAXJOBS      16   /* 任何时候最多的作业数量 */
#define MAXJID    1<<16   /* 最大作业ID */

/* 作业状态 */
#define UNDEF 0 /* 未定义 */
#define FG 1    /* 在前台运行 */
#define BG 2    /* 在后台运行 */
#define ST 3    /* 已停止 */

/*
 * 作业状态：FG（前台），BG（后台），ST（已停止）
 * 作业状态转换和启用动作：
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg命令
 *     ST -> BG  : bg命令
 *     BG -> FG  : fg命令
 * 最多只能有一个作业处于FG状态。
 */

/* 全局变量 */
extern char **environ;      /* 在libc中定义 */
char prompt[] = "tsh> ";    /* 命令行提示符（不要更改） */
int verbose = 0;            /* 如果为真，打印额外的输出 */
int nextjid = 1;            /* 下一个要分配的作业ID */
char sbuf[MAXLINE];         /* 用于组合sprintf消息 */

struct job_t {              /* 作业结构 */
    pid_t pid;              /* 作业PID */
    int jid;                /* 作业ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, 或 ST */
    char cmdline[MAXLINE];  /* 命令行 */
};
struct job_t jobs[MAXJOBS]; /* 作业列表 */
/* 结束全局变量 */


/* 函数原型 */

/* 这里是你将要实现的函数 */
void eval(char *cmdline);
int builtin_cmd(char **argv); // 已实现
void do_bgfg(char **argv); // 已实现
void waitfg(pid_t pid); // 完成，阻塞当前进程直到前台作业完成

void sigchld_handler(int sig); //  主要工作量
void sigtstp_handler(int sig); // 完成
void sigint_handler(int sig); // 完成

/* 这里是我们为你提供的辅助程序 */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - Shell的主程序
 */
int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* 发出提示（默认） */

    /* 将stderr重定向到stdout（这样驱动程序将在连接到stdout的管道上获得所有输出） */
    dup2(1, 2);

    /* 解析命令行 */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* 打印帮助信息 */
            usage();
        break;
        case 'v':             /* 发出额外的诊断信息 */
            verbose = 1;
        break;
        case 'p':             /* 不打印提示 */
            emit_prompt = 0;  /* 对于自动测试很方便 */
        break;
    default:
            usage();
    }
    }

    /* 安装信号处理程序 */

    /* 这些是你将需要实现的 */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* 子进程已终止或已停止 */

    /* 这个提供了一种优雅的方式来杀死shell */
    Signal(SIGQUIT, sigquit_handler);

    /* 初始化作业列表 */
    initjobs(jobs);

    /* 执行shell的读/评估循环 */
    while (1) {

    /* 读取命令行 */
    if (emit_prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }
    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
        app_error("fgets错误");
    if (feof(stdin)) { /* 文件结束（ctrl-d） */
        fflush(stdout);
        exit(0);
    }

    /* 评估命令行 */
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
    }

    exit(0); /* 控制永远不会到达这里 */
}
  
/*
 * eval - 评估用户刚刚输入的命令行
 *
 * 如果用户请求了一个内置命令（quit，jobs，bg或fg），则立即执行它。否则，fork一个子进程并在子进程的上下文中运行作业。如果作业在前台运行，等待它终止然后返回。注意：每个子进程必须有一个唯一的进程组ID，以便我们的后台子进程在我们在键盘上输入ctrl-c（ctrl-z）时不会从内核接收SIGINT（SIGTSTP）。
 */
void eval(char *cmdline)
{
    char *argv[MAXARGS];  // 参数数组
    int bg;
    pid_t pid;
    
    bg = parseline(cmdline, argv);// 解析命令行
    if (argv[0] == NULL) {
        return;  // 空命令行
    }
    if (cmdline == NULL || cmdline[0] == '\0') {
        return;
    }
    
    if (builtin_cmd(argv)) {
        return;// 内置命令直接执行
    }
    pid = fork();
    if(pid < 0){
        perror("fork error");
        return;
        }
    else if(pid > 0){// 父进程
        if(!bg){
        // addjob
        addjob(jobs,pid,FG,cmdline);
        waitfg(pid);// 阻塞父进程等待前台作业完成
        }else{
        addjob(jobs,pid,BG,cmdline);// 添加后台作业
        printf("[%d] (%d) %s\n", pid2jid(pid), pid, cmdline);
        }
        }else{// 子进程
        setpgid(0,0);
        if (execvp(argv[0], argv) < 0) { // 执行命令
        fprintf(stderr, "%s: Command not found.\n", argv[0]);
        exit(1);
        }
    }
    return;
}

/*
 * parseline - 解析命令行并构建argv数组。
 *
 * 单引号内的角色被视为单个参数。如果用户请求了一个BG作业，则返回true，如果请求了一个FG作业，则返回false。
 */
int parseline(const char *cmdline, char **argv)
{
    static char array[MAXLINE]; /* 保存命令行的本地副本 */
    char *buf = array;          /* 遍历命令行的指针 */
    char *delim;                /* 指向第一个空格分隔符 */
    int argc;                   /* 参数数量 */
    int bg;                     /* 后台作业 */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* 将尾随的'\n'替换为空格 */
    while (*buf && (*buf == ' ')) /* 忽略前导空格 */
    buf++;

    /* 构建argv列表 */
    argc = 0;
    if (*buf == '\'') {
    buf++;
    delim = strchr(buf, '\'');
    }
    else {
    delim = strchr(buf, ' ');
    }

    while (delim) {
    argv[argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' ')) /* 忽略空格 */
           buf++;

    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* 忽略空行 */
    return 1;

    /* 作业应该在后台运行吗？ */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
    argv[--argc] = NULL;
    }
    return bg;
}

/*
 * builtin_cmd - 如果用户输入了一个内置命令，则立即执行它。
 */
int builtin_cmd(char **argv)
{ // 完成
    if (strcmp(argv[0], "quit") == 0) {
        // printf("shell quit!\n");
        exit(0);  // 退出 shell
    }
    // 处理其他内置命令，例如 jobs, bg, fg
    else if (strcmp(argv[0], "jobs") == 0) {
        listjobs(jobs);  // 显示作业列表
        return 1;
    }else if(strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0){
        do_bgfg(argv);
        return 1;
    }
    // 初始化信号，创建子进程，管理子进程和父进程

    return 0;     /* 不是一个内置命令 */
}

/*
 * do_bgfg - 执行内置的bg和fg命令
 */
void do_bgfg(char **argv)
{
    struct job_t *job = NULL;
    if (argv[1] == NULL) {
        printf("bg/fg command requires PID or %%jobid argument\n");
        return;
    }
    // 判断是 %jid 还是 pid
    if (argv[1][0] == '%') {
        int jid = atoi(&argv[1][1]);
        job = getjobjid(jobs,jid);
        if (job == NULL) {
            printf("(%s): No such job\n", argv[1]);
            return;
        }
    }else if(isdigit(argv[1][0])){
        pid_t pid = atoi(argv[1]);
        job = getjobpid(jobs,pid);
         if (job == NULL) {
            printf("(%s): No such process\n", argv[1]);
            return;
        }
    }else{
        printf("bg/fg: argument must be a PID or %%jobid\n");
        return;
    }
     kill(-(job->pid), SIGCONT); // 继续作业
     if (strcmp(argv[0], "bg") == 0) { // bg 命令
        job->state = BG;
        printf("[%d] (%d) %s\n", job->jid, job->pid, job->cmdline);
    } else if (strcmp(argv[0], "fg") == 0) { // fg 命令
        job->state = FG;
        waitfg(job->pid);
    }
    return;
}

/*
 * waitfg - 阻塞直到进程pid不再是前台进程
 */
void waitfg(pid_t pid)
{
    while(pid == fgpid(jobs)){
        sleep(1);// 循环阻塞父进程
    }
    return;
}

/*****************
 * 信号处理程序
 *****************/

/*
 * sigchld_handler - 每当一个子作业终止（变成僵尸），或者因为它接收到SIGSTOP或SIGTSTP信号而停止时，内核会向shell发送一个SIGCHLD。
 * 处理程序收获所有可用的僵尸子作业，但不会等待任何其他当前运行的子作业终止。
 */
void sigchld_handler(int sig)
{
    // 保存和恢复errno确保信号处理函数不会干扰其他代码逻辑
    int olderrno = errno; // 保护 errno
    pid_t pid;
    int status;

    while (pid = waitpid(-1, &status, WNOHANG | WUNTRACED))
    {// 回收子进程
        if(WIFEXITED(status)){// 正常退出，删除作业
        deletejob(jobs,pid)
        }else if (WIFSIGNALED(status))
        {// 信号种植，打印信息并删除作业
             printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
             deletejob(jobs,pid);
        }else if(WIFSTOPPED(status)){
            // 信号暂停，更新作业状态
             struct job_t *job = getjobpid(jobs, pid);
             if(job){
                job->state = ST;// 暂停
                printf("Job [%d] (%d) stopped by signal %d\n", job->jid, pid, WSTOPSIG(status));
             }
        }
        
    }
    
    errno = olderrno;
    return;
}

/*
 * sigint_handler - 当用户在键盘上输入ctrl-c时，内核会向shell发送一个SIGINT。
 * 捕获它并将SIGINT发送给前台作业。
 */
void sigint_handler(int sig)
{
    pid_t pid = fgpid(jobs);
    if(pid != 0){
        printf("Job [%d] (%d) terminated by signal %d\n",
               pid2jid(pid), pid, sig);
        kill(-pid, SIGINT);
    }
    return;
}

/*
 * sigtstp_handler - 当用户在键盘上输入ctrl-z时，内核会向shell发送一个SIGTSTP。
 * 捕获它并通过发送SIGTSTP来暂停前台作业。
 */
void sigtstp_handler(int sig) 
{
    pid_t pid = fgpid(jobs);
    if(pid != 0){
        printf("Job [%d] (%d) stopped by signal %d\n",
               pid2jid(pid), pid, sig);
        kill(-pid, SIGTSTP);
    }
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - sigaction函数的包装器
 */
handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* 阻塞正在处理的信号类型 */
    action.sa_flags = SA_RESTART; /* 如果可能，重启系统调用 */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("信号错误");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - 驱动程序可以通过发送SIGQUIT信号优雅地终止子shell。
 */
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



