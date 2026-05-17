#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <gpiod.h>
#include "gpio.h"

#define MAX_GPIO_LINES 54

static struct gpiod_chip *chip = NULL;
static size_t chip_lines = 0;
/* One line request per GPIO offset. */
static struct gpiod_line_request *lines[MAX_GPIO_LINES] = { NULL };
static enum gpiod_line_direction line_directions[MAX_GPIO_LINES];
static enum gpiod_line_value line_values[MAX_GPIO_LINES];
static pull_type line_pulls[MAX_GPIO_LINES];
static int line_pull_configured[MAX_GPIO_LINES];

static int gpio_valid(int gpio_number)
{
    return gpio_number >= 0 &&
        gpio_number < MAX_GPIO_LINES &&
        (chip_lines == 0 || (size_t)gpio_number < chip_lines);
}

static int score_chip(struct gpiod_chip *candidate)
{
    static const unsigned int required[] = { 22, 23, 24, 27 };
    struct gpiod_chip_info *info;
    size_t num_lines;
    int score = 0;

    info = gpiod_chip_get_info(candidate);
    if (!info)
        return -1;

    num_lines = gpiod_chip_info_get_num_lines(info);
    if (num_lines <= required[sizeof(required) / sizeof(required[0]) - 1]) {
        gpiod_chip_info_free(info);
        return -1;
    }

    if (num_lines >= MAX_GPIO_LINES)
        score += 10;

    const char *name = gpiod_chip_info_get_name(info);
    const char *label = gpiod_chip_info_get_label(info);
    if ((name && strstr(name, "rp1")) || (label && strstr(label, "rp1")))
        score += 50;
    if ((name && strstr(name, "pinctrl")) || (label && strstr(label, "pinctrl")))
        score += 25;

    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        char expected[16];
        struct gpiod_line_info *line_info;
        const char *line_name;

        snprintf(expected, sizeof(expected), "GPIO%u", required[i]);
        line_info = gpiod_chip_get_line_info(candidate, required[i]);
        if (!line_info)
            continue;

        line_name = gpiod_line_info_get_name(line_info);
        if (line_name && strcmp(line_name, expected) == 0)
            score += 100;

        gpiod_line_info_free(line_info);
    }

    gpiod_chip_info_free(info);
    return score;
}

static struct gpiod_chip *open_best_gpiochip(void)
{
    struct gpiod_chip *best_chip = NULL;
    int best_score = -1;
    const char *override;

    override = getenv("PIBUS_GPIOCHIP");
    if (override && override[0]) {
        best_chip = gpiod_chip_open(override);
        if (!best_chip)
            perror("gpiod_chip_open");
        return best_chip;
    }

    for (int i = 0; i < 32; i++) {
        struct gpiod_chip *candidate;
        char path[32];
        int score;

        snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
        candidate = gpiod_chip_open(path);
        if (!candidate)
            continue;

        score = score_chip(candidate);
        if (score > best_score) {
            if (best_chip)
                gpiod_chip_close(best_chip);
            best_chip = candidate;
            best_score = score;
        } else {
            gpiod_chip_close(candidate);
        }
    }

    if (!best_chip) {
        best_chip = gpiod_chip_open("/dev/gpiochip0");
        if (!best_chip)
            perror("gpiod_chip_open");
    }

    return best_chip;
}

static enum gpiod_line_bias bias_from_pull(pull_type pt)
{
    switch (pt) {
        case PULL_DOWN:
            return GPIOD_LINE_BIAS_PULL_DOWN;
        case PULL_UP:
            return GPIOD_LINE_BIAS_PULL_UP;
        case PULL_NONE:
        default:
            return GPIOD_LINE_BIAS_DISABLED;
    }
}

int gpio_init(void)
{
    struct gpiod_chip_info *info;

    chip = open_best_gpiochip();
    if (!chip) {
        return -1;
    }

    info = gpiod_chip_get_info(chip);
    if (info) {
        chip_lines = gpiod_chip_info_get_num_lines(info);
        gpiod_chip_info_free(info);
    }

    return 0;
}

void gpio_cleanup(void)
{
    for (int i = 0; i < MAX_GPIO_LINES; i++) {
        if (lines[i]) {
            gpiod_line_request_release(lines[i]);
            lines[i] = NULL;
        }
    }
    if (chip) {
        gpiod_chip_close(chip);
        chip = NULL;
    }
}

/* internal helper: request a single GPIO line with given settings */
static struct gpiod_line_request *
request_single_line(unsigned int offset, enum gpiod_line_direction dir,
                    enum gpiod_line_value out_value, const char *consumer,
                    int use_bias)
{
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config   *line_cfg = NULL;
    struct gpiod_request_config *req_cfg = NULL;
    struct gpiod_line_request  *req = NULL;
    int ret;

    if (!chip)
        return NULL;

    settings = gpiod_line_settings_new();
    if (!settings) {
        perror("gpiod_line_settings_new");
        goto out;
    }

    ret = gpiod_line_settings_set_direction(settings, dir);
    if (ret < 0) {
        perror("gpiod_line_settings_set_direction");
        goto out;
    }

    if (use_bias && line_pull_configured[offset]) {
        ret = gpiod_line_settings_set_bias(settings,
                                           bias_from_pull(line_pulls[offset]));
        if (ret < 0) {
            perror("gpiod_line_settings_set_bias");
            goto out;
        }
    }

    if (dir == GPIOD_LINE_DIRECTION_OUTPUT) {
        ret = gpiod_line_settings_set_output_value(settings, out_value);
        if (ret < 0) {
            perror("gpiod_line_settings_set_output_value");
            goto out;
        }
    }

    line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        perror("gpiod_line_config_new");
        goto out;
    }

    ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
    if (ret < 0) {
        perror("gpiod_line_config_add_line_settings");
        goto out;
    }

    if (consumer) {
        req_cfg = gpiod_request_config_new();
        if (!req_cfg) {
            perror("gpiod_request_config_new");
            goto out;
        }
        gpiod_request_config_set_consumer(req_cfg, consumer);
    }

    req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!req) {
        perror("gpiod_chip_request_lines");
        goto out;
    }

out:
    if (req_cfg)
        gpiod_request_config_free(req_cfg);
    if (line_cfg)
        gpiod_line_config_free(line_cfg);
    if (settings)
        gpiod_line_settings_free(settings);

    return req;
}

static int request_gpio_line(int gpio_number, enum gpiod_line_direction dir)
{
    struct gpiod_line_request *req;
    int saved_errno;

    if (!gpio_valid(gpio_number))
        return -1;

    if (lines[gpio_number]) {
        gpiod_line_request_release(lines[gpio_number]);
        lines[gpio_number] = NULL;
    }

    req = request_single_line((unsigned int)gpio_number,
                              dir,
                              line_values[gpio_number],
                              "pibus",
                              1);
    saved_errno = errno;
    if (!req && line_pull_configured[gpio_number] &&
        (saved_errno == EINVAL ||
         saved_errno == ENOTSUP ||
         saved_errno == EOPNOTSUPP)) {
        fprintf(stderr, "gpio%d: pull bias failed, retrying without bias\n",
                gpio_number);
        req = request_single_line((unsigned int)gpio_number,
                                  dir,
                                  line_values[gpio_number],
                                  "pibus",
                                  0);
    }
    if (!req)
        return -1;

    lines[gpio_number] = req;
    line_directions[gpio_number] = dir;
    return 0;
}

void gpio_set_input(int gpio_number)
{
    if (!gpio_valid(gpio_number))
        return;

    if (request_gpio_line(gpio_number, GPIOD_LINE_DIRECTION_INPUT) < 0) {
        fprintf(stderr, "gpio_set_input(%d): failed\n", gpio_number);
        return;
    }
}

void gpio_set_output(int gpio_number)
{
    if (!gpio_valid(gpio_number))
        return;

    if (request_gpio_line(gpio_number, GPIOD_LINE_DIRECTION_OUTPUT) < 0) {
        fprintf(stderr, "gpio_set_output(%d): failed\n", gpio_number);
        return;
    }
}

int gpio_read(int gpio_number)
{
    if (!gpio_valid(gpio_number))
        return -1;
    if (!lines[gpio_number])
        return -1;

    enum gpiod_line_value value =
        gpiod_line_request_get_value(lines[gpio_number],
                                     (unsigned int)gpio_number);

    if (value == GPIOD_LINE_VALUE_ERROR) {
        perror("gpiod_line_request_get_value");
        return -1;
    }

    /* Preserve old behaviour: return 0 or 1 */
    return (value == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
}

void gpio_write(int gpio_number, int value)
{
    if (!gpio_valid(gpio_number))
        return;

    line_values[gpio_number] =
        value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;

    if (!lines[gpio_number])
        return;

    if (gpiod_line_request_set_value(lines[gpio_number],
                                     (unsigned int)gpio_number,
                                     line_values[gpio_number]) < 0) {
        perror("gpiod_line_request_set_value");
    }
}

void gpio_set_pull(int gpio_number, pull_type pt)
{
    if (!gpio_valid(gpio_number))
        return;

    line_pulls[gpio_number] = pt;
    line_pull_configured[gpio_number] = 1;

    if (lines[gpio_number] &&
        request_gpio_line(gpio_number, line_directions[gpio_number]) < 0) {
        fprintf(stderr, "gpio_set_pull(%d): failed\n", gpio_number);
    }
}

int uart_rx_fifo_empty(void)
{
    // Not applicable — handled by your serial driver, not GPIO.
    return 1;
}
