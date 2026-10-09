#!/bin/bash

sudo docker build -t speaker-stream:latest .

sudo docker run -it --rm -p 9000:9000 -v .:/app speaker-stream:latest
