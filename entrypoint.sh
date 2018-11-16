#!/bin/bash
echo "建立索引"
/usr/local/coreseek/bin/indexer --all 

echo "启动服务"
/usr/local/coreseek/bin/searchd --nodetach