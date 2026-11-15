const express = require('express');
const path = require('path');
const cors = require('cors');

const app = express();

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '../public')));

app.get('/', (req, res) => {
    res.json({ status: "Online", message: "API do Smartwatch de Saúde a funcionar com sucesso!" });
});

app.post('/readings', async (req, res) => {
    try {
        const { bpm, isAlert } = req.body;

        if (bpm === undefined) {
            return res.status(400).json({ error: "O campo 'bpm' é obrigatório." });
        }

        const novaLeitura = await prisma.heartRateLog.create({
            data: {
                bpm,
                isAlert: isAlert || false
            }
        });

        return res.status(201).json({
            message: "Leitura registada com sucesso!",
            dados: novaLeitura
        });
    } catch (error) {
        console.error("Erro ao salvar leitura:", error);
        return res.status(500).json({ error: "Erro interno no servidor." });
    }
});

const PORT = process.env.PORT || 3000;

app.listen(PORT, () => {
    console.log(`🚀 Servidor a rodar na porta ${PORT}`);
});