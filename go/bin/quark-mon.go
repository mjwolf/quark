package main

import (
	"fmt"
	"github.com/elastic/quark/go"
)

func main() {
	qq, err := quark.OpenQueue(quark.DefaultQueueAttr(), 64)
	if err != nil {
		panic(err)
	}
	pid1, ok := qq.Lookup(1)
	if ok {
		fmt.Printf("Yey for pid1\n %#v", pid1)
	}

	for {
		qevs, err := qq.GetEvents()
		if err != nil {
			panic(err)
		}
		for _, qev := range qevs {
			fmt.Printf("%#v", qev)
			if qev.Proc != nil {
				fmt.Printf(" %#v", qev.Proc)
			}
			fmt.Printf("\n")
		}
		if len(qevs) == 0 {
			err = qq.Block()
			if err != nil {
				panic(err)
			}
		}
	}
	qq.Close()
}
