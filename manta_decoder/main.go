package main

import (
	"encoding/json"
	"flag"
	"io"
	"log"
	"os"
	"reflect"
	"unicode/utf8"
	"strings"

	"github.com/dotabuff/manta"
	"github.com/dotabuff/manta/dota"
)

var errorType = reflect.TypeOf((*error)(nil)).Elem()

type outputState struct {
	encoder       *json.Encoder
	eclipseOnly   bool
	hasTick       bool
	currentTick   uint32
	currentBuffer []map[string]any
	tickMatched   bool
}

func newOutputState(enc *json.Encoder, eclipseOnly bool) *outputState {
	return &outputState{
		encoder:     enc,
		eclipseOnly: eclipseOnly,
	}
}

func (o *outputState) add(tick uint32, rec map[string]any, matches bool) error {
	if !o.eclipseOnly {
		return o.encoder.Encode(rec)
	}

	if !o.hasTick {
		o.hasTick = true
		o.currentTick = tick
	}

	if tick != o.currentTick {
		if err := o.flushTick(); err != nil {
			return err
		}
		o.currentTick = tick
	}

	o.currentBuffer = append(o.currentBuffer, rec)
	if matches {
		o.tickMatched = true
	}
	return nil
}

func (o *outputState) flushTick() error {
	if !o.hasTick {
		return nil
	}
	if o.tickMatched {
		for _, rec := range o.currentBuffer {
			if err := o.encoder.Encode(rec); err != nil {
				return err
			}
		}
	}
	o.currentBuffer = o.currentBuffer[:0]
	o.tickMatched = false
	return nil
}

func (o *outputState) flushFinal() error {
	return o.flushTick()
}

func lookupCombatLogName(parser *manta.Parser, idx uint32) string {
	if idx == 0 {
		return ""
	}
	name, ok := parser.LookupStringByIndex("CombatLogNames", int32(idx))
	if !ok {
		return ""
	}
	return name
}

func isLunaEclipseCast(parser *manta.Parser, m *dota.CMsgDOTACombatLogEntry) bool {
	t := m.GetType()
	if t != dota.DOTA_COMBATLOG_TYPES_DOTA_COMBATLOG_ABILITY &&
		t != dota.DOTA_COMBATLOG_TYPES_DOTA_COMBATLOG_ABILITY_TRIGGER {
		return false
	}
	inflictor := lookupCombatLogName(parser, m.GetInflictorName())
	if inflictor != "luna_eclipse" {
		return false
	}
	attacker := lookupCombatLogName(parser, m.GetAttackerName())
	return attacker == "npc_dota_hero_luna"
}

func bytesLookHumanReadable(b []byte) bool {
	if len(b) == 0 {
		return true
	}
	if !utf8.Valid(b) {
		return false
	}
	printable := 0
	for _, r := range string(b) {
		if r == '\n' || r == '\r' || r == '\t' || (r >= 32 && r <= 126) {
			printable++
		}
	}
	// Treat as human-readable if most characters are printable text-ish.
	return float64(printable)/float64(len([]rune(string(b)))) >= 0.85
}

func hasUnreadableBinaryPayload(v reflect.Value, depth int) bool {
	if depth > 4 || !v.IsValid() {
		return false
	}
	for v.Kind() == reflect.Pointer || v.Kind() == reflect.Interface {
		if v.IsNil() {
			return false
		}
		v = v.Elem()
	}

	switch v.Kind() {
	case reflect.Slice:
		if v.Type().Elem().Kind() == reflect.Uint8 {
			if v.Len() == 0 {
				return false
			}
			b := make([]byte, v.Len())
			reflect.Copy(reflect.ValueOf(b), v)
			return !bytesLookHumanReadable(b)
		}
		for i := 0; i < v.Len() && i < 8; i++ {
			if hasUnreadableBinaryPayload(v.Index(i), depth+1) {
				return true
			}
		}
	case reflect.Struct:
		for i := 0; i < v.NumField(); i++ {
			if hasUnreadableBinaryPayload(v.Field(i), depth+1) {
				return true
			}
		}
	case reflect.Map:
		iter := v.MapRange()
		i := 0
		for iter.Next() {
			if hasUnreadableBinaryPayload(iter.Value(), depth+1) {
				return true
			}
			i++
			if i >= 16 {
				break
			}
		}
	}
	return false
}

func registerAllCallbacks(
	parser *manta.Parser,
	out *outputState,
	registered *map[string]bool,
	wrote *int,
	includeBinary bool,
) {
	cbValue := reflect.ValueOf(parser.Callbacks)
	cbType := cbValue.Type()
	for i := 0; i < cbValue.NumMethod(); i++ {
		method := cbValue.Method(i)
		methodMeta := cbType.Method(i)
		methodName := methodMeta.Name
		if !strings.HasPrefix(methodName, "On") {
			continue
		}

		mt := method.Type()
		if mt.NumIn() != 1 || mt.NumOut() != 0 {
			continue
		}
		fnType := mt.In(0)
		if fnType.Kind() != reflect.Func {
			continue
		}
		if fnType.NumIn() != 1 || fnType.NumOut() != 1 || fnType.Out(0) != errorType {
			continue
		}

		if (*registered)[methodName] {
			continue
		}
		(*registered)[methodName] = true

		eventLabel := strings.TrimPrefix(methodName, "On")
		handler := reflect.MakeFunc(fnType, func(args []reflect.Value) []reflect.Value {
			if !includeBinary &&
				(strings.HasPrefix(eventLabel, "CNETMsg_") ||
					strings.HasPrefix(eventLabel, "CSVCMsg_") ||
					strings.HasPrefix(eventLabel, "CDemo")) {
				return []reflect.Value{reflect.Zero(errorType)}
			}

			payload := args[0].Interface()
			if !includeBinary && hasUnreadableBinaryPayload(reflect.ValueOf(payload), 0) {
				return []reflect.Value{reflect.Zero(errorType)}
			}

			(*wrote)++
			matches := false
			if eventLabel == "CMsgDOTACombatLogEntry" {
				if combat, ok := payload.(*dota.CMsgDOTACombatLogEntry); ok {
					matches = isLunaEclipseCast(parser, combat)
				}
			}
			record := map[string]any{
				"kind":     "callback",
				"name":     eventLabel,
				"tick":     parser.Tick,
				"net_tick": parser.NetTick,
				"payload":  payload,
			}
			if err := out.add(parser.Tick, record, matches); err != nil {
				return []reflect.Value{reflect.ValueOf(err)}
			}
			return []reflect.Value{reflect.Zero(errorType)}
		})
		method.Call([]reflect.Value{handler})
	}
}

func main() {
	demPath := flag.String("dem", "", "path to replay .dem file")
	outPath := flag.String("out", "-", "output path (.jsonl), or '-' for stdout")
	eclipseOnly := flag.Bool("eclipse", false, "only output events for ticks where Luna casts Eclipse")
	includeBinary := flag.Bool("include-binary", false, "include callbacks with unreadable binary payload bytes")
	flag.Parse()

	if *demPath == "" {
		log.Fatal("-dem is required")
	}

	in, err := os.Open(*demPath)
	if err != nil {
		log.Fatalf("open replay: %v", err)
	}
	defer in.Close()

	var out io.Writer = os.Stdout
	var outFile *os.File
	if *outPath != "-" {
		outFile, err = os.Create(*outPath)
		if err != nil {
			log.Fatalf("create output: %v", err)
		}
		defer outFile.Close()
		out = outFile
	}

	parser, err := manta.NewStreamParser(in)
	if err != nil {
		log.Fatalf("create parser: %v", err)
	}

	enc := json.NewEncoder(out)
	output := newOutputState(enc, *eclipseOnly)
	registered := make(map[string]bool)
	wrote := 0

	registerAllCallbacks(parser, output, &registered, &wrote, *includeBinary)

	parser.Callbacks.OnCMsgSource1LegacyGameEventList(func(m *dota.CMsgSource1LegacyGameEventList) error {
		for _, d := range m.GetDescriptors() {
			name := d.GetName()
			if name == "" || registered[name] {
				continue
			}
			registered[name] = true
			eventName := name
			parser.OnGameEvent(eventName, func(e *manta.GameEvent) error {
				wrote++
				record := map[string]any{
					"kind":       "game_event",
					"tick":       parser.Tick,
					"event_name": eventName,
				}
				return output.add(parser.Tick, record, false)
			})
		}
		return nil
	})

	if err := parser.Start(); err != nil {
		log.Fatalf("parse replay: %v", err)
	}
	if err := output.flushFinal(); err != nil {
		log.Fatalf("flush output: %v", err)
	}

	log.Printf("wrote %d events", wrote)
}
