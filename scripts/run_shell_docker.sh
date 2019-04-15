#!/usr/bin/env bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
TAG=${1:-latest}

echo "Running shell railrobotics/derail_fetchit:${TAG}"
docker run -it --rm \
    --network host --device /dev/snd --privileged \
    -v ${DIR}/../../:/root/ros/active/src \
    railrobotics/derail_fetchit:${TAG} \
    bash
# Use -v ${DIR}/../:/root/ros/active/src/derail-fetchit if you want to only
# mount the derail-fetchit folder
