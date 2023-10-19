#!/bin/sh -e

git init --bare $HOME/abcd-admin
install -m 755 post-receive $HOME/abcd-admin/hooks/
install -m 755 update $HOME/abcd-admin/hooks/

mkdir -p $HOME/bin
install -m 755 abcd-shell $HOME/bin/
