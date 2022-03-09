#!/bin/sh

if [ -d "/opt/imageDir" ]; then
  criu restore --file-locks --tcp-established --images-dir=/opt/imageDir --shell-job --restore-detached 2>/dev/null
else
  for i in {1..100}
  do
    # hack to bump up the pid 
    echo "nothing" > /dev/null
  done

  mkdir -p "/opt/imageDir"
  java -XX:+EnableCRIUSupport -cp /opt/$1/build/classes/java/main $1.$1
fi
