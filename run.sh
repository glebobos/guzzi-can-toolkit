#!/usr/bin/env bash

set -euo pipefail

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

log_info() {
  echo -e "${YELLOW}[INFO] $1${NC}"
}

log_success() {
  echo -e "${GREEN}[SUCCESS] $1${NC}"
}

log_error() {
  echo -e "${RED}[ERROR] $1${NC}" >&2
}

print_usage() {
  echo "Usage: ./run.sh <command> [options]"
  echo ""
  echo "Commands:"
  echo "  build-frontend     Build and compress Vite frontend into SPIFFS image"
  echo "  build-firmware     Compile ESP-IDF firmware and generate SPIFFS partition"
  echo "  build              Run both frontend and firmware builds"
  echo "  flash              Flash the firmware to XIAO ESP32-C3 / C6"
  echo "  monitor            Start ESP-IDF serial monitor"
  echo "  flash-monitor      Flash the firmware and start serial monitor"
  echo "  all                Build, flash, and monitor"
  echo "  clean              Remove build directories and docker volumes"
  echo "  shell              Start an interactive shell in the build container"
  echo "  menuconfig         Open ESP-IDF menuconfig configuration utility"
  echo ""
  echo "Options:"
  echo "  --port PORT        Override serial port (default: auto-detected)"
  echo "  --baud RATE        Override flashing/monitoring baud rate (default: 460800)"
  echo "  --dry-run          Print Docker commands without executing them"
}

detect_port() {
  for port in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0 /dev/ttyUSB1; do
    if [ -e "$port" ]; then
      echo "$port"
      return 0
    fi
  done
  log_error "No USB serial device found. Ensure board is connected and permissions are configured."
  exit 1
}

# Parse options
PORT=""
BAUD="460800"
DRY_RUN=false
COMMANDS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="$2"
      shift 2
      ;;
    --baud)
      BAUD="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=true
      shift
      ;;
    -*)
      log_error "Unknown option: $1"
      print_usage
      exit 1
      ;;
    *)
      COMMANDS+=("$1")
      shift
      ;;
  esac
done

if [ ${#COMMANDS[@]} -eq 0 ]; then
  log_error "No command specified."
  print_usage
  exit 1
fi

CMD="${COMMANDS[0]}"

run_cmd() {
  if [ "$DRY_RUN" = true ]; then
    echo -e "${YELLOW}[DRY-RUN]${NC} $@"
  else
    "$@"
  fi
}

needs_port() {
  case "$1" in
    flash|monitor|flash-monitor|all) return 0 ;;
    *) return 1 ;;
  esac
}

if needs_port "$CMD"; then
  if [ -z "$PORT" ]; then
    PORT=$(detect_port)
    log_info "Auto-detected serial port: $PORT"
  else
    log_info "Using specified serial port: $PORT"
  fi
fi

case "$CMD" in
  build-frontend)
    log_info "Building frontend web dashboard..."
    run_cmd docker compose up --build frontend-builder
    log_success "Frontend assets compiled and gzipped to SPIFFS image."
    ;;
    
  build-firmware)
    log_info "Compiling ESP-IDF Moto Guzzi CAN firmware..."
    run_cmd docker compose up --build firmware-builder
    log_success "Firmware compiled successfully."
    ;;
    
  build)
    log_info "Executing full compilation (Frontend + Firmware)..."
    run_cmd docker compose up --build frontend-builder
    run_cmd docker compose up --build firmware-builder
    log_success "All build targets compiled successfully."
    ;;
    
  flash)
    log_info "TIP: If flashing fails, hold BOOT button on XIAO board, re-plug USB cable, and release BOOT."
    log_info "Flashing firmware to device on $PORT at $BAUD baud..."
    run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd)/firmware:/project" -w /project guzzi-can-idf idf.py -p "$PORT" -b "$BAUD" flash
    log_success "Firmware flashed successfully."
    ;;
    
  monitor)
    log_info "Starting serial monitor on $PORT at $BAUD baud. Press Ctrl+] to exit."
    if [ "$DRY_RUN" = true ]; then
      echo -e "${YELLOW}[DRY-RUN]${NC} docker run -it --rm --privileged --device $PORT -v $(pwd)/firmware:/project -w /project guzzi-can-idf idf.py -p $PORT -b $BAUD monitor"
    else
      docker run -it --rm --privileged --device "$PORT" -v "$(pwd)/firmware:/project" -w /project guzzi-can-idf idf.py -p "$PORT" -b "$BAUD" monitor
    fi
    ;;
    
  flash-monitor)
    log_info "Flashing and starting serial monitor on $PORT..."
    if [ "$DRY_RUN" = true ]; then
      echo -e "${YELLOW}[DRY-RUN]${NC} docker run -it --rm --privileged --device $PORT -v $(pwd)/firmware:/project -w /project guzzi-can-idf idf.py -p $PORT -b $BAUD flash monitor"
    else
      docker run -it --rm --privileged --device "$PORT" -v "$(pwd)/firmware:/project" -w /project guzzi-can-idf idf.py -p "$PORT" -b "$BAUD" flash monitor
    fi
    ;;
    
  all)
    log_info "Starting full compilation and deployment..."
    run_cmd docker compose up --build frontend-builder
    run_cmd docker compose up --build firmware-builder
    log_info "Deploying to device on $PORT..."
    if [ "$DRY_RUN" = true ]; then
      echo -e "${YELLOW}[DRY-RUN]${NC} docker run -it --rm --privileged --device $PORT -v $(pwd)/firmware:/project -w /project guzzi-can-idf idf.py -p $PORT -b $BAUD flash monitor"
    else
      docker run -it --rm --privileged --device "$PORT" -v "$(pwd)/firmware:/project" -w /project guzzi-can-idf idf.py -p "$PORT" -b "$BAUD" flash monitor
    fi
    ;;
    
  clean)
    log_info "Cleaning build artifacts..."
    run_cmd rm -rf firmware/build
    run_cmd find firmware/spiffs_image -type f ! -name ".gitkeep" -delete
    run_cmd docker compose down -v
    log_success "Clean complete."
    ;;
    
  shell)
    log_info "Entering interactive build container shell..."
    DEVICE_ARG=""
    if [ -e "$PORT" ]; then
      DEVICE_ARG="--privileged --device $PORT"
    fi
    docker run -it --rm $DEVICE_ARG -v "$(pwd)/firmware:/project" -w /project guzzi-can-idf bash
    ;;
    
  menuconfig)
    log_info "Opening ESP-IDF menuconfig..."
    docker run -it --rm -v "$(pwd)/firmware:/project" -w /project guzzi-can-idf idf.py menuconfig
    ;;
    
  *)
    log_error "Unknown command: $CMD"
    print_usage
    exit 1
    ;;
esac
