PATH="$PATH:$HOME/.local/bin"

# Start tmux automatically on TTY1 (the physical screen)
if [ -z "$TMUX" ] && \
   [ "$TERM" != "screen" ] && \
   [ "$(tty)" = "/dev/tty1" ]; then
    tmux -2 attach-session -t 0 || \
    tmux -2 new-session -s 0
fi
