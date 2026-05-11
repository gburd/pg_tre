-- Initialize pg_tre extension in default database
CREATE EXTENSION IF NOT EXISTS pg_tre;

-- Create a sample table for testing
CREATE TABLE IF NOT EXISTS pg_tre_demo (
    id serial PRIMARY KEY,
    body text NOT NULL
);

-- Insert sample data
INSERT INTO pg_tre_demo (body) VALUES
    ('The PostgreSQL database system'),
    ('MySQL is also popular'),
    ('Oracle databases are expensive'),
    ('PostgreSQL supports approximate regex via pg_tre');

-- Create pg_tre index
CREATE INDEX pg_tre_demo_body_idx ON pg_tre_demo USING tre (body);

-- Verify installation
SELECT 'pg_tre extension installed successfully: ' || extversion 
FROM pg_extension WHERE extname = 'pg_tre';
