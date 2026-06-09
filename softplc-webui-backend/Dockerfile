FROM node:18-bookworm-slim AS deps

WORKDIR /app
COPY package.json package-lock.json* ./
RUN npm install --omit=dev

FROM node:18-bookworm-slim AS runtime
ENV NODE_ENV=production
WORKDIR /app
COPY --from=deps /app/node_modules ./node_modules
COPY . .

EXPOSE 4000
USER node
CMD ["node", "src/app.js"]
