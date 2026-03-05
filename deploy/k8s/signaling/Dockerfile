FROM golang:1.21-alpine AS builder
ENV GOPROXY=https://goproxy.cn,direct
WORKDIR /src
COPY src/signaling-server/go.mod src/signaling-server/go.sum ./
RUN go mod download
COPY src/signaling-server/ .
RUN CGO_ENABLED=0 go build -ldflags="-s -w" -o /signaling-server .

FROM alpine:3.19
RUN apk add --no-cache ca-certificates
COPY --from=builder /signaling-server /usr/local/bin/signaling-server
EXPOSE 8443
ENTRYPOINT ["signaling-server"]
