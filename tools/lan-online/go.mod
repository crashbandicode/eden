module edennsolan/backend

go 1.23.0

require (
	github.com/NextendoNetwork/nextendo-nex v0.0.0
	github.com/gorilla/websocket v1.5.3
	github.com/lxzan/gws v1.10.0
)

require github.com/klauspost/compress v1.17.9 // indirect

replace github.com/NextendoNetwork/nextendo-nex => ./third_party/nextendo-nex
