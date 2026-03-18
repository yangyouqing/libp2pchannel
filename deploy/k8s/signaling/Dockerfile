FROM golang:1.21-alpine AS builder
ENV GOPROXY=https://goproxy.cn,direct
WORKDIR /src
COPY src/signaling_server/go.mod src/signaling_server/go.sum ./
RUN go mod download
COPY src/signaling_server/ .
RUN CGO_ENABLED=0 go build -ldflags="-s -w" -o /signaling_server .

FROM alpine:3.19
RUN apk add --no-cache ca-certificates
COPY --from=builder /signaling_server /usr/local/bin/signaling_server
EXPOSE 8443
ENTRYPOINT ["signaling_server"]
