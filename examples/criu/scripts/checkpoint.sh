#!/bin/sh

echo "Building initial application image as app-temp"
docker build -f Dockerfile -t app-temp . || exit 1

echo "Running initial application"
docker run --name app-checkpoint --privileged app-temp
docker wait app-checkpoint || exit 1

echo "Committing container app-checkpoint as image app"
docker commit app-checkpoint app || exit 1

echo "Done building image app"
docker rm app-checkpoint
