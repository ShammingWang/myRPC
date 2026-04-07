# Protocol

`myRPC` 当前使用自定义 MRPC 二进制协议。

## Header Layout

请求与响应都使用固定 24 字节头：

```text
4 bytes   magic      固定为 "MRPC"
1 byte    version    当前为 1
1 byte    type       1=request, 2=response
2 bytes   status     请求时固定为 0，响应时表示状态码
4 bytes   method_len 请求方法名长度，响应固定为 0
4 bytes   body_len   负载长度
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
- `404`: method not found
- `500`: handler exception
- `503`: worker pool unavailable

## Notes

- 所有整数使用网络字节序编码。
- 响应顺序不保证与请求顺序一致，应以 `request_id` 匹配。
- 当前 payload 直接使用原始字节串，没有额外序列化层。
