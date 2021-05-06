## 信令服务器协议

### 服务地址
* 房间管理等业务服务采用HTTP协议，地址为：http://IP:PORT
* Peer To Peer的媒体协商、网络协商等服务采用WebSocket协议，地址为：ws://IP:PORT/signaling

### 通信协议
#### 房间管理
1. 查询某个房间的信息
```
接口地址：.../room/<roomid>
请求方式：GET
请求参数：无
返回结果：
{
    "status": 0,
    "message": "OK",
    "data": {
        "roomid": "xxx",
        "room_type": 0,
        "sessionids": [0, 1],
        "broadcaster_sessionid": 0
    }
}
```

2. 查询所有房间的信息
```
接口地址：.../rooms
请求方式：GET
请求参数：无
返回结果：
{
    "status": 0,
    "message": "OK",
    "data": {
        "xxx": 
        {
            "roomid": "xxx",
            "room_type": 0,
            "sessionids": [0, 1],
            "broadcaster_sessionid": 0
        },
        "yyy": 
        {
            "roomid": "yyy",
            "room_type": 1,
            "sessionids": [0, 1],
            "broadcaster_sessionid": -1
        }
    }
}
```

3. 打开房间
```
接口地址：.../room/open
请求方式：POST
请求参数：
{
    "sessionid": 0,
    "roomid": "xxx",
    "room_type": 0
}
返回结果：
{
    "status": 0,
    "message": "OK"
}
```

4. 加入房间
```
接口地址：.../room/join
请求方式：POST
请求参数：
{
    "sessionid": 0,
    "roomid": "xxx"
}
返回结果：
{
    "status": 0,
    "message": "OK",
    "data": {
        "roomid": "xxx",
        "room_type": 0,
        "sessionids": [0, 1],
        "broadcaster_sessionid": 0
    }
}
```

5. 离开房间
```
接口地址：.../room/leave
请求方式：POST
请求参数：
{
    "sessionid": 0
}
返回结果：
{
    "status": 0,
    "message": "OK"
}
```

#### P2P管理
1. RtcAgent通过WebSocket连接到信令服务器，将生成SessionId，服务端将登录成功信息返回给RtcAgent
```
{
    "command": "take_info",
    "type": "login_succeed",
    "sessionid": 0
}
```

2. Peer端主动发送SDP offer
```
{
    "command": "take_configuration",
    "type": "offer",
    "sdp": "xxx",
    "from": 0,
    "to": 1,
    "roomid": "xxx"
}
```

3. 信令服务器转发SDP offer
```
{
    "command": "take_configuration",
    "type": "forward_offer",
    "sdp": "xxx",
    "from": 0,
    "to": 1,
    "roomid": "xxx"
}
```

4. Peer端收到信令服务器转发的SDP offer，则发送SDP answer
```
{
    "command": "take_configuration",
    "type": "answer",
    "sdp": "xxx",
    "from": 0,
    "to": 1,
    "roomid": "xxx"
}
```

5. 信令服务器转发SDP answer
```
{
    "command": "take_configuration",
    "type": "forward_answer",
    "sdp": "xxx",
    "from": 0,
    "to": 1,
    "roomid": "xxx"
}
```

6. Peer端发送candidate到信令服务器
```
{
    "command": "take_candidate",
    "candidate": "xxx",
    "sdp_mid": "xxx",
    "sdp_mline_index": 0,
    "from": 0,
    "to": 1
}
```

7. 信令服务器直接转发candidate

#### WebSocket长连接心跳管理
1. 客户端每隔一段时间发送PING心跳包
```
{
    "command": "take_heartbeat",
    "type": "ping"
}
```

2. 服务端收到客户端心跳包，立即返回PONG心跳包
```
{
    "command": "take_heartbeat",
    "type": "pong"
}
```
