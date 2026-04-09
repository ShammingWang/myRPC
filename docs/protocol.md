# Protocol

`myRPC` 当前使用自定义 MRPC 二进制协议。

## Header Layout

请求与响应都使用固定 34 字节头：

```text
4 bytes   magic      固定为 "MRPC"
1 byte    version    当前为 1
1 byte    type       1=request, 2=response
1 byte    serialization 0=raw, 1=json
1 byte    reserved
2 bytes   status     请求时固定为 0，响应时表示状态码
4 bytes   method_len 请求方法名长度，响应固定为 0
4 bytes   body_len   负载长度
4 bytes   timeout_ms 请求超时，响应固定为 0
4 bytes   reserved
8 bytes   request_id 请求 ID
```

## Request Body

```text
method bytes + payload bytes
```

## Response Body

```text
payload bytes
```

## Status Code

- `0`: success
- `1001`: method not found
- `1002`: handler exception
- `1003`: invalid request
- `1004`: protocol error
- `1005`: serialization error
- `1006`: server overloaded
- `1007`: request timeout
- `2001`: network error
- `2002`: client timeout

## Notes

- 所有整数使用网络字节序编码。
- 响应顺序不保证与请求顺序一致，应以 `request_id` 匹配。
- 当前内置 `raw` 与 `json` 两种序列化类型。
- 方法名采用 `Service.Method` 形式，例如 `EchoService.Echo`。
