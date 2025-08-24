# NeoPixel Tibber Price Visualizer (ESP8266)
This project runs on an ESP8266 (e.g. Wemos D1 mini) with a NeoPixel ring/strip to visualize current and future Tibber electricity spot prices.
It fetches price data via Tibber’s GraphQL API, classifies them into levels, and shows a smooth spinning 3-LED “comet” animation with colors depending on the current and +3h forecast price.

![Result](./result.gif)
