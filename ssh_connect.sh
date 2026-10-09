#!/bin/bash

eval "$(ssh-agent -s)"

ssh-add ~/.ssh/kws_git

ssh -T git@github.com

