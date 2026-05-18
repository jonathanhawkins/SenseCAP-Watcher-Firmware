#ifndef CMD_H
#define CMD_H

/**
 * Initialize console command system
 *
 * Registers WiFi, device token, and reboot commands.
 * Starts the console REPL (Read-Eval-Print Loop).
 *
 * @return 0 on success
 */
int cmd_init(void);

#endif // CMD_H
