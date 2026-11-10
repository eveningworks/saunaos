/* Files through stdio and POSIX, directories, and <time.h>'s conversions of a fixed instant. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    const char *dir = "ft_dir";
    mkdir(dir, 0755);
    FILE *f = fopen("ft_dir/data.txt", "w");
    for (int i = 0; i < 500; i++) fprintf(f, "line %03d %s\n", i, i % 7 ? "plain" : "seventh");
    fclose(f);

    f = fopen("ft_dir/data.txt", "r");
    char buf[128];
    int lines = 0, sevens = 0;
    while (fgets(buf, sizeof buf, f)) {
        lines++;
        sevens += strstr(buf, "seventh") != NULL;
    }
    printf("fgets %d %d eof %d\n", lines, sevens, feof(f) != 0);
    fseek(f, -17, SEEK_END);
    fgets(buf, sizeof buf, f);
    printf("fseek [%.*s] ftell %ld\n", (int)strcspn(buf, "\n"), buf, ftell(f));
    rewind(f);
    int c, count = 0;
    while ((c = fgetc(f)) != EOF) count += c == '\n';
    ungetc('X', f);
    printf("fgetc %d ungetc %c\n", count, fgetc(f));
    fclose(f);

    f = fopen("ft_dir/data.bin", "w+b");
    double vals[64];
    for (int i = 0; i < 64; i++) vals[i] = i * 1.25;
    printf("fwrite %zu", fwrite(vals, sizeof vals[0], 64, f));
    fflush(f);
    fseek(f, 8 * 10, SEEK_SET);
    double got[4];
    size_t nread = fread(got, sizeof got[0], 4, f);
    printf(" fread %zu %.2f %.2f\n", nread, got[0], got[3]);
    fclose(f);

    int fd = open("ft_dir/raw", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    printf("write %zd", write(fd, "0123456789", 10));
    close(fd);
    fd = open("ft_dir/raw", O_RDWR | O_APPEND);
    write(fd, "abc", 3);
    lseek(fd, 4, SEEK_SET);
    char rb[8] = { 0 };
    ssize_t nr = read(fd, rb, 5);
    printf(" read %zd [%s]", nr, rb);
    close(fd);
    struct stat st;
    stat("ft_dir/raw", &st);
    printf(" size %lld reg %d\n", (long long)st.st_size, S_ISREG(st.st_mode) != 0);

    rename("ft_dir/raw", "ft_dir/renamed");
    errno = 0;
    int missing = open("ft_dir/raw", O_RDONLY);
    printf("open-missing %d enoent %d\n", missing, errno == ENOENT);
    DIR *d = opendir(dir);
    int ents = 0;
    struct dirent *e;
    char names[8][32];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (ents < 8) snprintf(names[ents], sizeof names[ents], "%s", e->d_name);
        ents++;
    }
    closedir(d);
    for (int i = 0; i < ents; i++)
        for (int j = i + 1; j < ents; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[32];
                memcpy(t, names[i], 32);
                memcpy(names[i], names[j], 32);
                memcpy(names[j], t, 32);
            }
    printf("readdir %d:", ents);
    for (int i = 0; i < ents; i++) printf(" %s", names[i]);
    printf("\n");
    unlink("ft_dir/data.txt");
    unlink("ft_dir/data.bin");
    unlink("ft_dir/renamed");
    printf("rmdir %d\n", rmdir(dir));

    time_t t = 1700000000;
    struct tm g;
    gmtime_r(&t, &g);
    printf("gmtime %d-%02d-%02d %02d:%02d:%02d wday %d yday %d\n", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
           g.tm_hour, g.tm_min, g.tm_sec, g.tm_wday, g.tm_yday);
    char tb[96];
    strftime(tb, sizeof tb, "%Y-%m-%dT%H:%M:%S %a %b %j %%", &g);
    printf("strftime %s\n", tb);
    struct tm leap = { .tm_year = 124, .tm_mon = 1, .tm_mday = 29, .tm_hour = 12 };
    printf("timegm %lld\n", (long long)timegm(&leap));
    struct tm norm = { .tm_year = 123, .tm_mon = 11, .tm_mday = 32 };
    timegm(&norm);
    printf("normalised %d-%02d-%02d\n", norm.tm_year + 1900, norm.tm_mon + 1, norm.tm_mday);
    struct timespec ts;
    printf("clock_gettime %d\n", clock_gettime(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_nsec < 1000000000L);
    return 0;
}
