library("ggplot2")

fcsv <- "/tmp/perf.csv"
bd <- read.csv(fcsv, col.names = c("Library", "Sockets", "Threads", "Rate"))
st <- bd[which(bd$Threads == 1),]
pt <- bd[which(bd$Library == "libphenom"),]

sockets_only = ggplot(st, aes(x = Sockets, y = Rate, colour = Library, fill = Library)) +
    geom_line()

ggsave(sockets_only, file = "sockets.png", width=6, height=4.5)

scale = ggplot(pt, aes(x = Sockets, y = Rate)) + geom_line() + facet_grid(. ~ Threads) +
    theme(axis.text.x = element_text(angle = 90, hjust = 1, size=5))

ggsave(scale, file = "scale.png", width=6, height=4.5)
