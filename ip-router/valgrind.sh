#!/bin/sh

valgrind --tool=memcheck --leak-check=yes -v ./ip-router alone ip-router.cf
